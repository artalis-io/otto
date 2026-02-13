/*
 * sh_pdf2struc.c - PDF Object Model, Xref, Page Tree, Main API
 *
 * Parses PDF structure: tokenizer, xref (classic + stream), ObjStm,
 * indirect reference resolution, page tree walking.
 */

#include "sh_pdf2struc_internal.h"
#include "sh_inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>

/* ============================================================================
 * Error Handling
 * ============================================================================ */

void pdf_set_error(ShPdf2strucCtx *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->error, PDF_MAX_ERROR_LEN, fmt, ap);
    va_end(ap);
}

/* ============================================================================
 * Arena Helpers
 * ============================================================================ */

char *pdf_arena_strndup(ShPdf2strucCtx *ctx, const char *s, size_t len)
{
    char *p = (char *)sh_arena_alloc(ctx->arena, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static PdfObj *pdf_alloc_obj(ShPdf2strucCtx *ctx)
{
    PdfObj *o = (PdfObj *)sh_arena_calloc(ctx->arena, 1, sizeof(PdfObj));
    return o;
}

/* ============================================================================
 * PDF Tokenizer
 * ============================================================================ */

static void scan_init(PdfScanner *s, const uint8_t *data, size_t size, size_t pos)
{
    s->data = data;
    s->size = size;
    s->pos  = pos;
}

static int scan_eof(const PdfScanner *s) { return s->pos >= s->size; }
static uint8_t scan_peek(const PdfScanner *s) {
    return s->pos < s->size ? s->data[s->pos] : 0;
}
static uint8_t scan_next(PdfScanner *s) {
    return s->pos < s->size ? s->data[s->pos++] : 0;
}

static void scan_skip_whitespace(PdfScanner *s)
{
    while (s->pos < s->size) {
        uint8_t c = s->data[s->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
            c == '\f' || c == '\0') {
            s->pos++;
        } else if (c == '%') {
            /* Skip comment to end of line */
            while (s->pos < s->size && s->data[s->pos] != '\n' &&
                   s->data[s->pos] != '\r')
                s->pos++;
        } else {
            break;
        }
    }
}

static int scan_match(PdfScanner *s, const char *str)
{
    size_t len = strlen(str);
    if (s->pos + len > s->size) return 0;
    if (memcmp(s->data + s->pos, str, len) == 0) {
        s->pos += len;
        return 1;
    }
    return 0;
}

/* Check if next chars match without consuming */
static int scan_looking_at(const PdfScanner *s, const char *str)
{
    size_t len = strlen(str);
    if (s->pos + len > s->size) return 0;
    return memcmp(s->data + s->pos, str, len) == 0;
}

static int is_pdf_delim(uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' ||
           c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

static int is_pdf_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '\f' || c == '\0';
}

/* ============================================================================
 * Object Parsing
 * ============================================================================ */

/* Forward declaration (non-static — used by sh_pdf2struc_text.c via internal header) */

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Parse literal string: (...) with balanced parens and escape sequences */
static PdfObj *pdf_parse_string(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    s->pos++; /* skip ( */
    size_t start = s->pos;
    int depth = 1;

    /* First pass: find end to compute length */
    size_t scan_pos = s->pos;
    while (scan_pos < s->size && depth > 0) {
        uint8_t c = s->data[scan_pos++];
        if (c == '\\' && scan_pos < s->size) scan_pos++;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
    }

    size_t raw_len = scan_pos - start - 1; /* exclude closing ) */
    uint8_t *buf = (uint8_t *)sh_arena_alloc(ctx->arena, raw_len + 1);
    if (!buf) return NULL;

    /* Second pass: decode escapes */
    size_t out = 0;
    depth = 1;
    while (s->pos < s->size && depth > 0) {
        uint8_t c = s->data[s->pos++];
        if (c == '\\' && s->pos < s->size) {
            uint8_t esc = s->data[s->pos++];
            switch (esc) {
                case 'n': buf[out++] = '\n'; break;
                case 'r': buf[out++] = '\r'; break;
                case 't': buf[out++] = '\t'; break;
                case 'b': buf[out++] = '\b'; break;
                case 'f': buf[out++] = '\f'; break;
                case '(': buf[out++] = '('; break;
                case ')': buf[out++] = ')'; break;
                case '\\': buf[out++] = '\\'; break;
                case '\r':
                    if (s->pos < s->size && s->data[s->pos] == '\n') s->pos++;
                    break;
                case '\n': break;
                default:
                    /* Octal */
                    if (esc >= '0' && esc <= '7') {
                        int val = esc - '0';
                        if (s->pos < s->size && s->data[s->pos] >= '0' &&
                            s->data[s->pos] <= '7') {
                            val = val * 8 + (s->data[s->pos++] - '0');
                            if (s->pos < s->size && s->data[s->pos] >= '0' &&
                                s->data[s->pos] <= '7')
                                val = val * 8 + (s->data[s->pos++] - '0');
                        }
                        buf[out++] = (uint8_t)(val & 0xFF);
                    } else {
                        buf[out++] = esc;
                    }
                    break;
            }
        } else if (c == '(') {
            depth++;
            buf[out++] = c;
        } else if (c == ')') {
            depth--;
            if (depth > 0) buf[out++] = c;
        } else {
            buf[out++] = c;
        }
    }
    buf[out] = '\0';

    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;
    obj->type = PDF_OBJ_STRING;
    obj->string_val.data = buf;
    obj->string_val.len = out;
    return obj;
}

/* Parse hex string: <...> */
static PdfObj *pdf_parse_hexstring(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    s->pos++; /* skip < */
    size_t start = s->pos;

    /* Find closing > */
    while (s->pos < s->size && s->data[s->pos] != '>')
        s->pos++;

    size_t hex_len = s->pos - start;
    if (s->pos < s->size) s->pos++; /* skip > */

    /* Count hex digits (skip whitespace) */
    size_t ndigits = 0;
    for (size_t i = 0; i < hex_len; i++) {
        uint8_t c = s->data[start + i];
        if (hex_digit(c) >= 0) ndigits++;
    }

    size_t byte_len = (ndigits + 1) / 2;
    uint8_t *buf = (uint8_t *)sh_arena_alloc(ctx->arena, byte_len + 1);
    if (!buf) return NULL;

    size_t out = 0;
    int high = -1;
    for (size_t i = 0; i < hex_len; i++) {
        int d = hex_digit(s->data[start + i]);
        if (d < 0) continue;
        if (high < 0) {
            high = d;
        } else {
            buf[out++] = (uint8_t)((high << 4) | d);
            high = -1;
        }
    }
    if (high >= 0) buf[out++] = (uint8_t)(high << 4);
    buf[out] = '\0';

    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;
    obj->type = PDF_OBJ_HEXSTRING;
    obj->string_val.data = buf;
    obj->string_val.len = out;
    return obj;
}

/* Parse name: /Name */
static PdfObj *pdf_parse_name(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    s->pos++; /* skip / */
    size_t start = s->pos;

    while (s->pos < s->size) {
        uint8_t c = s->data[s->pos];
        if (is_pdf_ws(c) || is_pdf_delim(c)) break;
        s->pos++;
    }

    size_t len = s->pos - start;

    /* Decode #XX sequences */
    char *buf = (char *)sh_arena_alloc(ctx->arena, len + 1);
    if (!buf) return NULL;

    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        if (s->data[start + i] == '#' && i + 2 < len) {
            int h = hex_digit(s->data[start + i + 1]);
            int l = hex_digit(s->data[start + i + 2]);
            if (h >= 0 && l >= 0) {
                buf[out++] = (char)((h << 4) | l);
                i += 2;
                continue;
            }
        }
        buf[out++] = (char)s->data[start + i];
    }
    buf[out] = '\0';

    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;
    obj->type = PDF_OBJ_NAME;
    obj->name_val = buf;
    return obj;
}

/* Parse number (int or real) */
static PdfObj *pdf_parse_number(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    size_t start = s->pos;
    int has_dot = 0;
    if (s->pos < s->size && (s->data[s->pos] == '+' || s->data[s->pos] == '-'))
        s->pos++;
    while (s->pos < s->size) {
        uint8_t c = s->data[s->pos];
        if (c == '.') { has_dot = 1; s->pos++; }
        else if (c >= '0' && c <= '9') s->pos++;
        else break;
    }

    size_t len = s->pos - start;
    char tmp[64];
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, s->data + start, len);
    tmp[len] = '\0';

    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;

    if (has_dot) {
        obj->type = PDF_OBJ_REAL;
        obj->real_val = strtod(tmp, NULL);
    } else {
        obj->type = PDF_OBJ_INT;
        obj->int_val = strtoll(tmp, NULL, 10);
    }
    return obj;
}

/* Parse array: [...] */
static PdfObj *pdf_parse_array(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    s->pos++; /* skip [ */
    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;
    obj->type = PDF_OBJ_ARRAY;
    obj->array_val.count = 0;
    obj->array_val.capacity = 8;
    obj->array_val.items = (PdfObj **)sh_arena_alloc(ctx->arena,
                                8 * sizeof(PdfObj *));
    if (!obj->array_val.items) return NULL;

    int limit = 100000;
    while (!scan_eof(s) && limit-- > 0) {
        scan_skip_whitespace(s);
        if (scan_eof(s) || scan_peek(s) == ']') { s->pos++; break; }

        PdfObj *item = pdf_parse_obj(ctx, s);
        if (!item) break;

        if (obj->array_val.count >= obj->array_val.capacity) {
            int new_cap = obj->array_val.capacity * 2;
            PdfObj **new_items = (PdfObj **)sh_arena_alloc(ctx->arena,
                                     (size_t)new_cap * sizeof(PdfObj *));
            if (!new_items) return NULL;
            memcpy(new_items, obj->array_val.items,
                   (size_t)obj->array_val.count * sizeof(PdfObj *));
            obj->array_val.items = new_items;
            obj->array_val.capacity = new_cap;
        }
        obj->array_val.items[obj->array_val.count++] = item;
    }
    return obj;
}

/* Parse dict: << ... >> (and stream if followed by 'stream') */
static PdfObj *pdf_parse_dict(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    s->pos += 2; /* skip << */
    PdfObj *obj = pdf_alloc_obj(ctx);
    if (!obj) return NULL;
    obj->type = PDF_OBJ_DICT;

    PdfDict *d = &obj->dict_val;
    d->count = 0;
    d->capacity = 8;
    d->keys = (const char **)sh_arena_alloc(ctx->arena,
                   8 * sizeof(const char *));
    d->vals = (PdfObj **)sh_arena_alloc(ctx->arena,
                   8 * sizeof(PdfObj *));
    if (!d->keys || !d->vals) return NULL;

    int limit = 10000;
    while (!scan_eof(s) && limit-- > 0) {
        scan_skip_whitespace(s);
        if (s->pos + 1 < s->size && s->data[s->pos] == '>' &&
            s->data[s->pos + 1] == '>') {
            s->pos += 2;
            break;
        }
        if (scan_eof(s)) break;

        /* Key must be a name */
        if (scan_peek(s) != '/') break;
        PdfObj *key_obj = pdf_parse_name(ctx, s);
        if (!key_obj) break;

        scan_skip_whitespace(s);
        PdfObj *val = pdf_parse_obj(ctx, s);
        if (!val) {
            /* Create null value */
            val = pdf_alloc_obj(ctx);
            if (val) val->type = PDF_OBJ_NULL;
            else break;
        }

        if (d->count >= d->capacity) {
            int new_cap = d->capacity * 2;
            const char **new_keys = (const char **)sh_arena_alloc(ctx->arena,
                                         (size_t)new_cap * sizeof(const char *));
            PdfObj **new_vals = (PdfObj **)sh_arena_alloc(ctx->arena,
                                     (size_t)new_cap * sizeof(PdfObj *));
            if (!new_keys || !new_vals) return NULL;
            memcpy(new_keys, d->keys, (size_t)d->count * sizeof(const char *));
            memcpy(new_vals, d->vals, (size_t)d->count * sizeof(PdfObj *));
            d->keys = new_keys;
            d->vals = new_vals;
            d->capacity = new_cap;
        }
        d->keys[d->count] = key_obj->name_val;
        d->vals[d->count] = val;
        d->count++;
    }

    /* Check for stream */
    scan_skip_whitespace(s);
    if (scan_looking_at(s, "stream")) {
        s->pos += 6; /* skip "stream" */
        /* Skip \r\n or \n after "stream" */
        if (s->pos < s->size && s->data[s->pos] == '\r') s->pos++;
        if (s->pos < s->size && s->data[s->pos] == '\n') s->pos++;

        size_t stream_start = s->pos;

        /* Get /Length */
        PdfObj *len_obj = pdf_dict_get(d, "Length");
        size_t stream_len = 0;
        if (len_obj) {
            PdfObj *resolved = pdf_resolve(ctx, len_obj);
            if (resolved && resolved->type == PDF_OBJ_INT)
                stream_len = (size_t)resolved->int_val;
            else if (resolved && resolved->type == PDF_OBJ_REAL)
                stream_len = (size_t)resolved->real_val;
        }

        /* If /Length was 0 or missing, search for endstream */
        if (stream_len == 0 || stream_start + stream_len > s->size) {
            const uint8_t *end = (const uint8_t *)memmem(
                s->data + stream_start,
                s->size - stream_start,
                "endstream", 9);
            if (end) {
                stream_len = (size_t)(end - s->data - stream_start);
                /* Trim trailing whitespace before endstream */
                while (stream_len > 0 &&
                       (s->data[stream_start + stream_len - 1] == '\r' ||
                        s->data[stream_start + stream_len - 1] == '\n'))
                    stream_len--;
            }
        }

        obj->type = PDF_OBJ_STREAM;
        obj->stream_val.dict = *d;
        obj->stream_val.offset = stream_start;
        obj->stream_val.length = stream_len;

        /* Advance past stream data + endstream */
        s->pos = stream_start + stream_len;
        scan_skip_whitespace(s);
        scan_match(s, "endstream");
    }

    return obj;
}

/* Main object parser - dispatches by first character */
PdfObj *pdf_parse_obj(ShPdf2strucCtx *ctx, PdfScanner *s)
{
    scan_skip_whitespace(s);
    if (scan_eof(s)) return NULL;

    uint8_t c = scan_peek(s);

    /* Dict or hex string */
    if (c == '<') {
        if (s->pos + 1 < s->size && s->data[s->pos + 1] == '<')
            return pdf_parse_dict(ctx, s);
        return pdf_parse_hexstring(ctx, s);
    }

    /* Array */
    if (c == '[') return pdf_parse_array(ctx, s);

    /* Name */
    if (c == '/') return pdf_parse_name(ctx, s);

    /* Literal string */
    if (c == '(') return pdf_parse_string(ctx, s);

    /* Boolean or null */
    if (scan_looking_at(s, "true")) {
        s->pos += 4;
        PdfObj *obj = pdf_alloc_obj(ctx);
        if (obj) { obj->type = PDF_OBJ_BOOL; obj->bool_val = 1; }
        return obj;
    }
    if (scan_looking_at(s, "false")) {
        s->pos += 5;
        PdfObj *obj = pdf_alloc_obj(ctx);
        if (obj) { obj->type = PDF_OBJ_BOOL; obj->bool_val = 0; }
        return obj;
    }
    if (scan_looking_at(s, "null")) {
        s->pos += 4;
        PdfObj *obj = pdf_alloc_obj(ctx);
        if (obj) obj->type = PDF_OBJ_NULL;
        return obj;
    }

    /* Number or indirect reference (N M R) or indirect object (N M obj) */
    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        PdfObj *num = pdf_parse_number(ctx, s);
        if (!num) return NULL;

        if (num->type == PDF_OBJ_INT) {
            /* Check for "M R" (indirect ref) or "M obj" (indirect object) */
            size_t save2 = s->pos;
            scan_skip_whitespace(s);
            if (!scan_eof(s) && s->data[s->pos] >= '0' &&
                s->data[s->pos] <= '9') {
                PdfObj *gen = pdf_parse_number(ctx, s);
                if (gen && gen->type == PDF_OBJ_INT) {
                    scan_skip_whitespace(s);
                    if (scan_looking_at(s, "R") &&
                        (s->pos + 1 >= s->size ||
                         is_pdf_ws(s->data[s->pos + 1]) ||
                         is_pdf_delim(s->data[s->pos + 1]))) {
                        s->pos++; /* skip R */
                        PdfObj *ref = pdf_alloc_obj(ctx);
                        if (!ref) return NULL;
                        ref->type = PDF_OBJ_REF;
                        ref->ref_val.obj_num = (int)num->int_val;
                        ref->ref_val.gen_num = (int)gen->int_val;
                        return ref;
                    }
                    if (scan_looking_at(s, "obj")) {
                        s->pos += 3;
                        scan_skip_whitespace(s);
                        PdfObj *inner = pdf_parse_obj(ctx, s);
                        scan_skip_whitespace(s);
                        scan_match(s, "endobj");
                        return inner;
                    }
                }
                s->pos = save2;
            } else {
                s->pos = save2;
            }
        }
        return num;
    }

    /* Unknown token - skip one byte */
    s->pos++;
    return NULL;
}

/* ============================================================================
 * Dict/Array Helpers
 * ============================================================================ */

PdfObj *pdf_dict_get(PdfDict *d, const char *key)
{
    if (!d) return NULL;
    for (int i = 0; i < d->count; i++) {
        if (d->keys[i] && strcmp(d->keys[i], key) == 0)
            return d->vals[i];
    }
    return NULL;
}

static PdfDict *pdf_get_dict_ptr(ShPdf2strucCtx *ctx, PdfObj *obj)
{
    if (!obj) return NULL;
    obj = pdf_resolve(ctx, obj);
    if (!obj) return NULL;
    if (obj->type == PDF_OBJ_DICT) return &obj->dict_val;
    if (obj->type == PDF_OBJ_STREAM) return &obj->stream_val.dict;
    return NULL;
}

int64_t pdf_dict_get_int(ShPdf2strucCtx *ctx, PdfDict *d, const char *key, int64_t def)
{
    PdfObj *v = pdf_resolve(ctx, pdf_dict_get(d, key));
    if (!v) return def;
    if (v->type == PDF_OBJ_INT) return v->int_val;
    if (v->type == PDF_OBJ_REAL) return (int64_t)v->real_val;
    return def;
}

double pdf_dict_get_real(ShPdf2strucCtx *ctx, PdfDict *d, const char *key, double def)
{
    PdfObj *v = pdf_resolve(ctx, pdf_dict_get(d, key));
    if (!v) return def;
    if (v->type == PDF_OBJ_REAL) return v->real_val;
    if (v->type == PDF_OBJ_INT) return (double)v->int_val;
    return def;
}

const char *pdf_dict_get_name(ShPdf2strucCtx *ctx, PdfDict *d, const char *key)
{
    PdfObj *v = pdf_resolve(ctx, pdf_dict_get(d, key));
    if (!v || v->type != PDF_OBJ_NAME) return NULL;
    return v->name_val;
}

int pdf_array_len(PdfObj *arr)
{
    if (!arr || arr->type != PDF_OBJ_ARRAY) return 0;
    return arr->array_val.count;
}

PdfObj *pdf_array_get(PdfObj *arr, int idx)
{
    if (!arr || arr->type != PDF_OBJ_ARRAY) return NULL;
    if (idx < 0 || idx >= arr->array_val.count) return NULL;
    return arr->array_val.items[idx];
}

static double pdf_obj_as_num(PdfObj *obj, double def)
{
    if (!obj) return def;
    if (obj->type == PDF_OBJ_INT) return (double)obj->int_val;
    if (obj->type == PDF_OBJ_REAL) return obj->real_val;
    return def;
}

/* ============================================================================
 * Object Resolution (indirect refs)
 * ============================================================================ */

static PdfObj *pdf_parse_at_offset(ShPdf2strucCtx *ctx, size_t offset);
static PdfObj *pdf_parse_from_objstm(ShPdf2strucCtx *ctx, int stm_obj, int stm_idx);

PdfObj *pdf_resolve(ShPdf2strucCtx *ctx, PdfObj *obj)
{
    if (!obj) return NULL;
    if (obj->type != PDF_OBJ_REF) return obj;

    int num = obj->ref_val.obj_num;
    if (num < 0 || num >= ctx->obj_cache_size) return NULL;

    /* Return cached */
    if (ctx->obj_cache[num]) return ctx->obj_cache[num];

    /* Look up xref */
    if (num >= ctx->xref_size) return NULL;
    PdfXrefEntry *xe = &ctx->xref[num];
    if (!xe->in_use) return NULL;

    PdfObj *resolved = NULL;
    if (xe->compressed) {
        resolved = pdf_parse_from_objstm(ctx, xe->stm_obj, xe->stm_idx);
    } else {
        resolved = pdf_parse_at_offset(ctx, xe->offset);
    }

    if (resolved) ctx->obj_cache[num] = resolved;
    return resolved;
}

static PdfObj *pdf_parse_at_offset(ShPdf2strucCtx *ctx, size_t offset)
{
    if (offset >= ctx->data_size) return NULL;
    PdfScanner s;
    scan_init(&s, ctx->data, ctx->data_size, offset);
    return pdf_parse_obj(ctx, &s);
}

/* ============================================================================
 * Stream Decompression
 * ============================================================================ */

static int pdf_apply_png_predictor(uint8_t *data, size_t len, int columns)
{
    if (columns <= 0) return -1;
    int row_bytes = columns;
    int stride = row_bytes + 1; /* 1 byte predictor tag per row */
    int nrows = (int)(len / (size_t)stride);
    if (nrows <= 0) return -1;

    for (int r = 0; r < nrows; r++) {
        uint8_t *row = data + r * stride;
        uint8_t tag = row[0];
        uint8_t *pix = row + 1;
        uint8_t *prev = (r > 0) ? (data + (r - 1) * stride + 1) : NULL;

        switch (tag) {
            case 0: /* None */
                break;
            case 1: /* Sub */
                for (int i = 1; i < row_bytes; i++)
                    pix[i] = (uint8_t)(pix[i] + pix[i - 1]);
                break;
            case 2: /* Up */
                if (prev) {
                    for (int i = 0; i < row_bytes; i++)
                        pix[i] = (uint8_t)(pix[i] + prev[i]);
                }
                break;
            case 3: /* Average */
                for (int i = 0; i < row_bytes; i++) {
                    uint8_t left = (i > 0) ? pix[i - 1] : 0;
                    uint8_t up = prev ? prev[i] : 0;
                    pix[i] = (uint8_t)(pix[i] + ((left + up) >> 1));
                }
                break;
            case 4: { /* Paeth */
                for (int i = 0; i < row_bytes; i++) {
                    int a = (i > 0) ? pix[i - 1] : 0;
                    int b = prev ? prev[i] : 0;
                    int c = (prev && i > 0) ? prev[i - 1] : 0;
                    int p = a + b - c;
                    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                    uint8_t pr;
                    if (pa <= pb && pa <= pc) pr = (uint8_t)a;
                    else if (pb <= pc) pr = (uint8_t)b;
                    else pr = (uint8_t)c;
                    pix[i] = (uint8_t)(pix[i] + pr);
                }
                break;
            }
            default:
                break;
        }
    }

    /* Remove predictor tags: compact rows */
    size_t out = 0;
    for (int r = 0; r < nrows; r++) {
        memmove(data + out, data + r * stride + 1, (size_t)row_bytes);
        out += (size_t)row_bytes;
    }
    return (int)out;
}

uint8_t *pdf_decompress_stream(ShPdf2strucCtx *ctx, PdfObj *stream_obj,
                                size_t *out_len)
{
    if (!stream_obj || stream_obj->type != PDF_OBJ_STREAM) return NULL;

    PdfDict *d = &stream_obj->stream_val.dict;
    size_t offset = stream_obj->stream_val.offset;
    size_t length = stream_obj->stream_val.length;

    if (offset + length > ctx->data_size) {
        pdf_set_error(ctx, "stream data out of bounds");
        return NULL;
    }

    const uint8_t *raw = ctx->data + offset;

    /* Check filter */
    const char *filter = pdf_dict_get_name(ctx, d, "Filter");
    if (!filter) {
        /* Uncompressed stream */
        uint8_t *buf = (uint8_t *)sh_arena_alloc(ctx->arena, length + 1);
        if (!buf) return NULL;
        memcpy(buf, raw, length);
        buf[length] = '\0';
        *out_len = length;
        return buf;
    }

    if (strcmp(filter, "FlateDecode") != 0) {
        pdf_set_error(ctx, "unsupported filter: %s", filter);
        return NULL;
    }

    /* Get expected decompressed size hint */
    int64_t dl_hint = pdf_dict_get_int(ctx, d, "DL", 0);
    if (dl_hint <= 0) dl_hint = (int64_t)length * 10;
    if (dl_hint > PDF_MAX_DECOMPRESS) dl_hint = PDF_MAX_DECOMPRESS;
    size_t decomp_cap = (size_t)dl_hint;

    uint8_t *decomp = (uint8_t *)sh_arena_alloc(ctx->arena, decomp_cap + 1);
    if (!decomp) {
        pdf_set_error(ctx, "OOM for stream decompression");
        return NULL;
    }

    size_t actual = 0;
    SHStatus st = sh_inflate(raw, length, decomp, decomp_cap, &actual);
    if (st != SH_OK) {
        /* Try raw deflate */
        st = sh_inflate_raw(raw, length, decomp, decomp_cap, &actual);
        if (st != SH_OK) {
            pdf_set_error(ctx, "FlateDecode decompression failed");
            return NULL;
        }
    }
    decomp[actual] = '\0';

    /* Check for PNG predictor in DecodeParms */
    PdfObj *dp_obj = pdf_resolve(ctx, pdf_dict_get(d, "DecodeParms"));
    if (dp_obj) {
        PdfDict *dp = pdf_get_dict_ptr(ctx, dp_obj);
        if (dp) {
            int predictor = (int)pdf_dict_get_int(ctx, dp, "Predictor", 1);
            if (predictor >= 10) {
                int columns = (int)pdf_dict_get_int(ctx, dp, "Columns", 1);
                int new_len = pdf_apply_png_predictor(decomp, actual, columns);
                if (new_len > 0) actual = (size_t)new_len;
            }
        }
    }

    *out_len = actual;
    return decomp;
}

/* ============================================================================
 * Xref Parsing — Classic
 * ============================================================================ */

static size_t pdf_find_startxref(ShPdf2strucCtx *ctx)
{
    /* Search last 1024 bytes for "startxref" */
    size_t search_start = ctx->data_size > 1024 ? ctx->data_size - 1024 : 0;
    for (size_t i = ctx->data_size; i > search_start; i--) {
        if (i >= 9 && memcmp(ctx->data + i - 9, "startxref", 9) == 0) {
            /* Parse the number after startxref */
            PdfScanner s;
            scan_init(&s, ctx->data, ctx->data_size, i);
            scan_skip_whitespace(&s);
            if (!scan_eof(&s)) {
                PdfObj *num = pdf_parse_number(ctx, &s);
                if (num && num->type == PDF_OBJ_INT)
                    return (size_t)num->int_val;
            }
        }
    }
    return 0;
}

static int pdf_parse_classic_xref(ShPdf2strucCtx *ctx, size_t offset)
{
    if (offset >= ctx->data_size) return -1;

    PdfScanner s;
    scan_init(&s, ctx->data, ctx->data_size, offset);
    scan_skip_whitespace(&s);

    if (!scan_match(&s, "xref")) return -1;
    scan_skip_whitespace(&s);

    /* Parse subsections */
    int limit = 10000;
    while (!scan_eof(&s) && limit-- > 0) {
        scan_skip_whitespace(&s);
        if (scan_looking_at(&s, "trailer")) break;

        /* start count */
        PdfObj *start_obj = pdf_parse_number(ctx, &s);
        if (!start_obj || start_obj->type != PDF_OBJ_INT) break;
        scan_skip_whitespace(&s);
        PdfObj *count_obj = pdf_parse_number(ctx, &s);
        if (!count_obj || count_obj->type != PDF_OBJ_INT) break;

        int start = (int)start_obj->int_val;
        int count = (int)count_obj->int_val;
        if (count < 0 || count > PDF_MAX_XREF_SIZE) break;

        /* Expand xref table if needed */
        int needed = start + count;
        if (needed > ctx->xref_size) {
            if (needed > PDF_MAX_OBJECTS) {
                pdf_set_error(ctx, "xref too large: %d", needed);
                return -1;
            }
            PdfXrefEntry *new_xref = (PdfXrefEntry *)sh_arena_calloc(
                ctx->arena, (size_t)needed, sizeof(PdfXrefEntry));
            if (!new_xref) return -1;
            if (ctx->xref && ctx->xref_size > 0)
                memcpy(new_xref, ctx->xref,
                       (size_t)ctx->xref_size * sizeof(PdfXrefEntry));
            ctx->xref = new_xref;
            ctx->xref_size = needed;
        }

        for (int i = 0; i < count; i++) {
            scan_skip_whitespace(&s);
            /* Each entry: NNNNNNNNNN NNNNN n/f */
            PdfObj *off_obj = pdf_parse_number(ctx, &s);
            scan_skip_whitespace(&s);
            PdfObj *gen_obj = pdf_parse_number(ctx, &s);
            scan_skip_whitespace(&s);

            char tag = 'f';
            if (!scan_eof(&s)) tag = (char)scan_next(&s);

            if (off_obj && gen_obj && off_obj->type == PDF_OBJ_INT &&
                gen_obj->type == PDF_OBJ_INT) {
                int idx = start + i;
                if (idx >= 0 && idx < ctx->xref_size &&
                    !ctx->xref[idx].in_use) {
                    ctx->xref[idx].offset = (size_t)off_obj->int_val;
                    ctx->xref[idx].gen = (int)gen_obj->int_val;
                    ctx->xref[idx].in_use = (tag == 'n') ? 1 : 0;
                }
            }
        }
    }

    /* Parse trailer */
    scan_skip_whitespace(&s);
    if (!scan_match(&s, "trailer")) {
        pdf_set_error(ctx, "missing trailer");
        return -1;
    }
    scan_skip_whitespace(&s);
    PdfObj *trailer = pdf_parse_obj(ctx, &s);
    if (!trailer) return -1;

    PdfDict *td = pdf_get_dict_ptr(ctx, trailer);
    if (!td) return -1;

    /* Follow /Prev chain */
    int64_t prev = pdf_dict_get_int(ctx, td, "Prev", 0);
    if (prev > 0) {
        pdf_parse_classic_xref(ctx, (size_t)prev);
    }

    /* Store trailer root/info for later */
    PdfObj *root = pdf_dict_get(td, "Root");
    if (root) {
        /* Store at obj_cache slot 0 with special meaning */
        /* We use a separate approach: just parse the root now */
        ctx->obj_cache[0] = trailer; /* temporary: trailer at slot 0 */
    }

    return 0;
}

/* ============================================================================
 * Xref Parsing — Xref Stream (PDF 1.5+)
 * ============================================================================ */

static int pdf_parse_xref_stream(ShPdf2strucCtx *ctx, size_t offset)
{
    if (offset >= ctx->data_size) return -1;

    PdfScanner s;
    scan_init(&s, ctx->data, ctx->data_size, offset);
    PdfObj *obj = pdf_parse_obj(ctx, &s);
    if (!obj || obj->type != PDF_OBJ_STREAM) {
        pdf_set_error(ctx, "expected xref stream at offset %zu", offset);
        return -1;
    }

    PdfDict *d = &obj->stream_val.dict;

    /* Verify it's an xref stream */
    const char *type = pdf_dict_get_name(ctx, d, "Type");
    if (!type || strcmp(type, "XRef") != 0) {
        pdf_set_error(ctx, "not an xref stream");
        return -1;
    }

    int64_t size_val = pdf_dict_get_int(ctx, d, "Size", 0);
    if (size_val <= 0 || size_val > PDF_MAX_OBJECTS) {
        pdf_set_error(ctx, "xref stream size invalid: %lld", (long long)size_val);
        return -1;
    }

    /* /W array: field widths */
    PdfObj *w_arr = pdf_dict_get(d, "W");
    if (!w_arr || w_arr->type != PDF_OBJ_ARRAY || pdf_array_len(w_arr) != 3) {
        pdf_set_error(ctx, "xref stream /W invalid");
        return -1;
    }
    int w0 = (int)pdf_obj_as_num(pdf_array_get(w_arr, 0), 0);
    int w1 = (int)pdf_obj_as_num(pdf_array_get(w_arr, 1), 0);
    int w2 = (int)pdf_obj_as_num(pdf_array_get(w_arr, 2), 0);
    int entry_size = w0 + w1 + w2;
    if (entry_size <= 0 || entry_size > 20) {
        pdf_set_error(ctx, "xref stream entry size invalid");
        return -1;
    }

    /* Decompress stream */
    size_t decomp_len = 0;
    uint8_t *decomp = pdf_decompress_stream(ctx, obj, &decomp_len);
    if (!decomp) return -1;

    /* Expand xref table */
    int needed = (int)size_val;
    if (needed > ctx->xref_size) {
        PdfXrefEntry *new_xref = (PdfXrefEntry *)sh_arena_calloc(
            ctx->arena, (size_t)needed, sizeof(PdfXrefEntry));
        if (!new_xref) return -1;
        if (ctx->xref && ctx->xref_size > 0)
            memcpy(new_xref, ctx->xref,
                   (size_t)ctx->xref_size * sizeof(PdfXrefEntry));
        ctx->xref = new_xref;
        ctx->xref_size = needed;
    }

    /* /Index array (default: [0 Size]) */
    PdfObj *index_arr = pdf_dict_get(d, "Index");
    int subsections[256]; /* pairs of (start, count) */
    int nsubs = 0;
    if (index_arr && index_arr->type == PDF_OBJ_ARRAY) {
        int n = pdf_array_len(index_arr);
        for (int i = 0; i + 1 < n && nsubs < 128; i += 2) {
            subsections[nsubs * 2] = (int)pdf_obj_as_num(
                pdf_array_get(index_arr, i), 0);
            subsections[nsubs * 2 + 1] = (int)pdf_obj_as_num(
                pdf_array_get(index_arr, i + 1), 0);
            nsubs++;
        }
    } else {
        subsections[0] = 0;
        subsections[1] = (int)size_val;
        nsubs = 1;
    }

    /* Decode entries */
    size_t pos = 0;
    for (int sub = 0; sub < nsubs; sub++) {
        int start = subsections[sub * 2];
        int count = subsections[sub * 2 + 1];
        for (int i = 0; i < count && pos + (size_t)entry_size <= decomp_len; i++) {
            /* Read fields */
            uint64_t f0 = 0, f1 = 0, f2 = 0;
            for (int j = 0; j < w0; j++)
                f0 = (f0 << 8) | decomp[pos++];
            for (int j = 0; j < w1; j++)
                f1 = (f1 << 8) | decomp[pos++];
            for (int j = 0; j < w2; j++)
                f2 = (f2 << 8) | decomp[pos++];

            /* Default type is 1 if w0 == 0 */
            if (w0 == 0) f0 = 1;

            int idx = start + i;
            if (idx < 0 || idx >= ctx->xref_size) continue;
            if (ctx->xref[idx].in_use) continue; /* Don't overwrite newer entries */

            switch (f0) {
                case 0: /* Free */
                    ctx->xref[idx].in_use = 0;
                    break;
                case 1: /* In-use, uncompressed */
                    ctx->xref[idx].offset = (size_t)f1;
                    ctx->xref[idx].gen = (int)f2;
                    ctx->xref[idx].in_use = 1;
                    break;
                case 2: /* Compressed in ObjStm */
                    ctx->xref[idx].compressed = 1;
                    ctx->xref[idx].stm_obj = (int)f1;
                    ctx->xref[idx].stm_idx = (int)f2;
                    ctx->xref[idx].in_use = 1;
                    break;
            }
        }
    }

    /* Store this as trailer equivalent (has /Root) */
    PdfObj *trailer_equiv = pdf_alloc_obj(ctx);
    if (trailer_equiv) {
        trailer_equiv->type = PDF_OBJ_DICT;
        trailer_equiv->dict_val = *d;
        if (!ctx->obj_cache[0]) ctx->obj_cache[0] = trailer_equiv;
    }

    /* Follow /Prev chain */
    int64_t prev = pdf_dict_get_int(ctx, d, "Prev", 0);
    if (prev > 0) {
        /* Previous could be classic or stream xref */
        PdfScanner ps;
        scan_init(&ps, ctx->data, ctx->data_size, (size_t)prev);
        scan_skip_whitespace(&ps);
        if (scan_looking_at(&ps, "xref"))
            pdf_parse_classic_xref(ctx, (size_t)prev);
        else
            pdf_parse_xref_stream(ctx, (size_t)prev);
    }

    return 0;
}

/* ============================================================================
 * Object Stream (ObjStm)
 * ============================================================================ */

static PdfObj *pdf_parse_from_objstm(ShPdf2strucCtx *ctx, int stm_obj, int stm_idx)
{
    if (stm_obj < 0 || stm_obj >= ctx->xref_size) return NULL;

    /* First resolve the ObjStm stream object itself */
    PdfXrefEntry *stm_xe = &ctx->xref[stm_obj];
    if (!stm_xe->in_use || stm_xe->compressed) return NULL;

    PdfObj *stm = pdf_parse_at_offset(ctx, stm_xe->offset);
    if (!stm || stm->type != PDF_OBJ_STREAM) return NULL;

    PdfDict *d = &stm->stream_val.dict;
    const char *type = pdf_dict_get_name(ctx, d, "Type");
    if (!type || strcmp(type, "ObjStm") != 0) return NULL;

    int n = (int)pdf_dict_get_int(ctx, d, "N", 0);
    int first = (int)pdf_dict_get_int(ctx, d, "First", 0);
    if (n <= 0 || n > PDF_MAX_OBJSTM_OBJS || first < 0) return NULL;
    if (stm_idx < 0 || stm_idx >= n) return NULL;

    /* Decompress */
    size_t decomp_len = 0;
    uint8_t *decomp = pdf_decompress_stream(ctx, stm, &decomp_len);
    if (!decomp) return NULL;

    /* Parse (obj_num, offset) pairs from header */
    PdfScanner hs;
    scan_init(&hs, decomp, decomp_len, 0);

    int *obj_nums = (int *)sh_arena_alloc(ctx->arena, (size_t)n * sizeof(int));
    int *offsets = (int *)sh_arena_alloc(ctx->arena, (size_t)n * sizeof(int));
    if (!obj_nums || !offsets) return NULL;

    for (int i = 0; i < n; i++) {
        scan_skip_whitespace(&hs);
        PdfObj *num = pdf_parse_number(ctx, &hs);
        scan_skip_whitespace(&hs);
        PdfObj *off = pdf_parse_number(ctx, &hs);
        obj_nums[i] = num ? (int)num->int_val : 0;
        offsets[i] = off ? (int)off->int_val : 0;
    }

    /* Parse object at stm_idx */
    size_t obj_offset = (size_t)(first + offsets[stm_idx]);
    if (obj_offset >= decomp_len) return NULL;

    PdfScanner os;
    scan_init(&os, decomp, decomp_len, obj_offset);
    return pdf_parse_obj(ctx, &os);
}

/* ============================================================================
 * Page Tree
 * ============================================================================ */

static void pdf_collect_pages(ShPdf2strucCtx *ctx, PdfObj *node,
                               PdfObj *inherited_resources,
                               PdfObj *inherited_mediabox)
{
    node = pdf_resolve(ctx, node);
    if (!node) return;

    PdfDict *d = pdf_get_dict_ptr(ctx, node);
    if (!d) return;

    const char *type = pdf_dict_get_name(ctx, d, "Type");

    /* Inherit resources and mediabox */
    PdfObj *res = pdf_dict_get(d, "Resources");
    if (res) inherited_resources = res;
    PdfObj *mbox = pdf_dict_get(d, "MediaBox");
    if (mbox) inherited_mediabox = mbox;

    if (type && strcmp(type, "Pages") == 0) {
        PdfObj *kids = pdf_resolve(ctx, pdf_dict_get(d, "Kids"));
        if (kids && kids->type == PDF_OBJ_ARRAY) {
            int n = pdf_array_len(kids);
            for (int i = 0; i < n; i++) {
                pdf_collect_pages(ctx, pdf_array_get(kids, i),
                                   inherited_resources, inherited_mediabox);
            }
        }
    } else if (type && strcmp(type, "Page") == 0) {
        if (ctx->page_count >= PDF_MAX_PAGES) return;

        PdfPageInfo *pi = &ctx->pages[ctx->page_count];
        pi->resources = inherited_resources;
        pi->contents = pdf_dict_get(d, "Contents");
        pi->mediabox = inherited_mediabox;

        /* Parse MediaBox for dimensions */
        PdfObj *mb = pdf_resolve(ctx, pi->mediabox);
        if (mb && mb->type == PDF_OBJ_ARRAY && pdf_array_len(mb) >= 4) {
            double x0 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(mb, 0)), 0);
            double y0 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(mb, 1)), 0);
            double x1 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(mb, 2)), 612);
            double y1 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(mb, 3)), 792);
            pi->width = x1 - x0;
            pi->height = y1 - y0;
        } else {
            pi->width = 612.0;
            pi->height = 792.0;
        }

        /* Use CropBox if present */
        PdfObj *crop = pdf_resolve(ctx, pdf_dict_get(d, "CropBox"));
        if (crop && crop->type == PDF_OBJ_ARRAY && pdf_array_len(crop) >= 4) {
            double x0 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(crop, 0)), 0);
            double y0 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(crop, 1)), 0);
            double x1 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(crop, 2)), 612);
            double y1 = pdf_obj_as_num(pdf_resolve(ctx, pdf_array_get(crop, 3)), 792);
            pi->width = x1 - x0;
            pi->height = y1 - y0;
        }

        ctx->page_count++;
    }
}

/* ============================================================================
 * Encryption Check
 * ============================================================================ */

static int pdf_check_encrypted(ShPdf2strucCtx *ctx)
{
    PdfObj *trailer = ctx->obj_cache[0];
    if (!trailer) return 0;
    PdfDict *d = pdf_get_dict_ptr(ctx, trailer);
    if (!d) return 0;
    PdfObj *encrypt = pdf_dict_get(d, "Encrypt");
    return (encrypt != NULL) ? 1 : 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

ShPdf2strucCtx *sh_pdf2struc_create(void)
{
    SHArena *arena = sh_arena_create(16 * 1024 * 1024); /* 16 MB initial */
    if (!arena) return NULL;

    ShPdf2strucCtx *ctx = (ShPdf2strucCtx *)sh_arena_calloc(arena, 1,
                               sizeof(ShPdf2strucCtx));
    if (!ctx) { sh_arena_free(arena); return NULL; }

    ctx->arena = arena;
    ctx->error[0] = '\0';
    return ctx;
}

void sh_pdf2struc_destroy(ShPdf2strucCtx *ctx)
{
    if (!ctx) return;
    SHArena *arena = ctx->arena;
    sh_arena_free(arena);
}

void sh_pdf2struc_opts_default(ShPdf2strucOpts *opt)
{
    if (!opt) return;
    opt->emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opt->origin_top_left = 1;
    opt->approx_widths = 1;
    opt->merge_y_epsilon = 1.0;
    opt->merge_x_gap = 3.0;
}

const char *sh_pdf2struc_last_error(const ShPdf2strucCtx *ctx)
{
    if (!ctx) return "null context";
    return ctx->error;
}

static ShPdf2strucStatus pdf_do_extract(ShPdf2strucCtx *ctx,
                                         const ShPdf2strucOpts *opt,
                                         ShPdf2strucCallback cb, void *user)
{
    ShPdf2strucOpts default_opt;
    if (!opt) {
        sh_pdf2struc_opts_default(&default_opt);
        opt = &default_opt;
    }

    /* Verify PDF header */
    if (ctx->data_size < 8 || memcmp(ctx->data, "%PDF-", 5) != 0) {
        pdf_set_error(ctx, "not a PDF file");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    /* Allocate object cache */
    ctx->obj_cache_size = PDF_MAX_OBJECTS;
    ctx->obj_cache = (PdfObj **)sh_arena_calloc(ctx->arena,
                          (size_t)ctx->obj_cache_size, sizeof(PdfObj *));
    if (!ctx->obj_cache) return SH_PDF2STRUC_ERR_OOM;

    /* Find startxref */
    size_t xref_offset = pdf_find_startxref(ctx);
    if (xref_offset == 0) {
        pdf_set_error(ctx, "cannot find startxref");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    /* Parse xref (classic or stream) */
    PdfScanner xs;
    scan_init(&xs, ctx->data, ctx->data_size, xref_offset);
    scan_skip_whitespace(&xs);

    if (scan_looking_at(&xs, "xref")) {
        if (pdf_parse_classic_xref(ctx, xref_offset) != 0)
            return SH_PDF2STRUC_ERR_INVALID_PDF;
    } else {
        if (pdf_parse_xref_stream(ctx, xref_offset) != 0)
            return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    /* Check for encryption */
    if (pdf_check_encrypted(ctx)) {
        pdf_set_error(ctx, "encrypted PDFs not supported");
        return SH_PDF2STRUC_ERR_UNSUPPORTED;
    }

    /* Get /Root -> /Pages */
    PdfObj *trailer = ctx->obj_cache[0];
    if (!trailer) {
        pdf_set_error(ctx, "no trailer found");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    PdfDict *td = pdf_get_dict_ptr(ctx, trailer);
    if (!td) return SH_PDF2STRUC_ERR_INVALID_PDF;

    PdfObj *root = pdf_resolve(ctx, pdf_dict_get(td, "Root"));
    if (!root) {
        pdf_set_error(ctx, "no /Root in trailer");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    PdfDict *root_d = pdf_get_dict_ptr(ctx, root);
    if (!root_d) return SH_PDF2STRUC_ERR_INVALID_PDF;

    PdfObj *pages = pdf_dict_get(root_d, "Pages");
    if (!pages) {
        pdf_set_error(ctx, "no /Pages in root");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    /* Allocate page info array */
    ctx->pages = (PdfPageInfo *)sh_arena_calloc(ctx->arena, PDF_MAX_PAGES,
                      sizeof(PdfPageInfo));
    if (!ctx->pages) return SH_PDF2STRUC_ERR_OOM;
    ctx->page_count = 0;

    /* Walk page tree */
    pdf_collect_pages(ctx, pages, NULL, NULL);
    if (ctx->page_count == 0) {
        pdf_set_error(ctx, "no pages found");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    /* Allocate run buffer */
    ctx->run_capacity = 4096;
    ctx->runs = (PdfTextRun *)sh_arena_alloc(ctx->arena,
                     (size_t)ctx->run_capacity * sizeof(PdfTextRun));
    if (!ctx->runs) return SH_PDF2STRUC_ERR_OOM;
    ctx->run_count = 0;

    /* Allocate font table */
    ctx->fonts = (PdfFont *)sh_arena_calloc(ctx->arena, PDF_MAX_FONTS,
                      sizeof(PdfFont));
    if (!ctx->fonts) return SH_PDF2STRUC_ERR_OOM;
    ctx->font_count = 0;

    /* Extract text (implemented in sh_pdf2struc_text.c) */
    ShPdf2strucStatus st = pdf_extract_text(ctx, opt);
    if (st != SH_PDF2STRUC_OK) return st;

    /* Group and emit */
    return pdf_group_runs(ctx, opt, cb, user);
}

ShPdf2strucStatus sh_pdf2struc_extract_mem(
    ShPdf2strucCtx *ctx, const uint8_t *data, size_t size,
    const ShPdf2strucOpts *opt, ShPdf2strucCallback cb, void *user)
{
    if (!ctx) return SH_PDF2STRUC_ERR_INVALID_PDF;
    if (!data || size == 0) {
        pdf_set_error(ctx, "null or empty data");
        return SH_PDF2STRUC_ERR_INVALID_PDF;
    }

    ctx->data = data;
    ctx->data_size = size;

    return pdf_do_extract(ctx, opt, cb, user);
}

ShPdf2strucStatus sh_pdf2struc_extract_file(
    ShPdf2strucCtx *ctx, const char *path,
    const ShPdf2strucOpts *opt, ShPdf2strucCallback cb, void *user)
{
    if (!ctx) return SH_PDF2STRUC_ERR_INVALID_PDF;
    if (!path) {
        pdf_set_error(ctx, "null path");
        return SH_PDF2STRUC_ERR_IO;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        pdf_set_error(ctx, "cannot open: %s", path);
        return SH_PDF2STRUC_ERR_IO;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        pdf_set_error(ctx, "empty file: %s", path);
        return SH_PDF2STRUC_ERR_IO;
    }

    uint8_t *buf = (uint8_t *)sh_arena_alloc(ctx->arena, (size_t)sz);
    if (!buf) {
        fclose(f);
        return SH_PDF2STRUC_ERR_OOM;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    ctx->data = buf;
    ctx->data_size = rd;

    return pdf_do_extract(ctx, opt, cb, user);
}
