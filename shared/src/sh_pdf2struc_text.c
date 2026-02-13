/*
 * sh_pdf2struc_text.c - Content Stream Parser, Font Decoding, Geometry, Grouping
 *
 * Phases 2-4 of PDF text extraction:
 * - Content stream tokenizer + operator dispatch
 * - Graphics state stack (q/Q, cm)
 * - Text state machine (BT/ET, Tf, Tm, Td, TJ, etc.)
 * - Font decoding: WinAnsi + CID/Identity-H with ToUnicode CMap
 * - Geometry: TRM computation, bbox, coordinate conversion
 * - Block grouping: merge adjacent runs into words
 */

#include "sh_pdf2struc_internal.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

/* ============================================================================
 * WinAnsi Encoding Table
 * ============================================================================ */

static const uint16_t WINANSI_TO_UNICODE[256] = {
    /* 0x00-0x0F */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 0x10-0x1F */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /* 0x20-0x2F */ 0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,
                    0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    /* 0x30-0x3F */ 0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
                    0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    /* 0x40-0x4F */ 0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
                    0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    /* 0x50-0x5F */ 0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
                    0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    /* 0x60-0x6F */ 0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
                    0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    /* 0x70-0x7F */ 0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
                    0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    /* 0x80-0x8F */ 0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
                    0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
    /* 0x90-0x9F */ 0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
                    0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178,
    /* 0xA0-0xAF */ 0x00A0,0x00A1,0x00A2,0x00A3,0x00A4,0x00A5,0x00A6,0x00A7,
                    0x00A8,0x00A9,0x00AA,0x00AB,0x00AC,0x00AD,0x00AE,0x00AF,
    /* 0xB0-0xBF */ 0x00B0,0x00B1,0x00B2,0x00B3,0x00B4,0x00B5,0x00B6,0x00B7,
                    0x00B8,0x00B9,0x00BA,0x00BB,0x00BC,0x00BD,0x00BE,0x00BF,
    /* 0xC0-0xCF */ 0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,
                    0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
    /* 0xD0-0xDF */ 0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,
                    0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
    /* 0xE0-0xEF */ 0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,
                    0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
    /* 0xF0-0xFF */ 0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,
                    0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
};

/* ============================================================================
 * UTF-8 Encoding
 * ============================================================================ */

static int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* ============================================================================
 * Matrix Operations
 * ============================================================================ */

static PdfMatrix mat_identity(void)
{
    return (PdfMatrix){1, 0, 0, 1, 0, 0};
}

static PdfMatrix mat_mul(PdfMatrix a, PdfMatrix b)
{
    PdfMatrix r;
    r.a = a.a * b.a + a.b * b.c;
    r.b = a.a * b.b + a.b * b.d;
    r.c = a.c * b.a + a.d * b.c;
    r.d = a.c * b.b + a.d * b.d;
    r.e = a.e * b.a + a.f * b.c + b.e;
    r.f = a.e * b.b + a.f * b.d + b.f;
    return r;
}

static void mat_apply(const PdfMatrix *m, double x, double y,
                       double *ox, double *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

/* ============================================================================
 * Font Parsing
 * ============================================================================ */

static int hex_val(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Parse hex-encoded codepoint from CMap data */
static uint32_t parse_cmap_hex(const uint8_t *data, size_t len)
{
    uint32_t val = 0;
    for (size_t i = 0; i < len; i++) {
        int d = hex_val(data[i]);
        if (d < 0) continue;
        val = (val << 4) | (uint32_t)d;
    }
    return val;
}

/* Parse a ToUnicode CMap stream */
static void parse_tounicode(ShPdf2strucCtx *ctx, PdfFont *font,
                              const uint8_t *data, size_t len)
{
    font->tounicode = (PdfToUnicodeEntry *)sh_arena_alloc(ctx->arena,
                           PDF_MAX_TOUNICODE * sizeof(PdfToUnicodeEntry));
    if (!font->tounicode) return;
    font->tounicode_count = 0;
    font->has_tounicode = 1;

    const uint8_t *end = data + len;
    const uint8_t *p = data;

    while (p < end) {
        /* Find beginbfchar or beginbfrange */
        const uint8_t *bfchar = (const uint8_t *)memmem(
            p, (size_t)(end - p), "beginbfchar", 11);
        const uint8_t *bfrange = (const uint8_t *)memmem(
            p, (size_t)(end - p), "beginbfrange", 12);

        /* Pick whichever comes first */
        const uint8_t *next = NULL;
        int is_range = 0;
        if (bfchar && (!bfrange || bfchar < bfrange)) {
            next = bfchar + 11;
            is_range = 0;
        } else if (bfrange) {
            next = bfrange + 12;
            is_range = 1;
        } else {
            break;
        }
        p = next;

        if (!is_range) {
            /* beginbfchar: pairs of <src> <dst> */
            int limit = 10000;
            while (p < end && limit-- > 0) {
                /* Skip whitespace */
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r'))
                    p++;
                if (p >= end) break;

                /* Check for endbfchar */
                if (p + 10 <= end && memcmp(p, "endbfchar", 9) == 0) {
                    p += 9;
                    break;
                }

                /* Parse <src> */
                if (*p != '<') break;
                p++;
                const uint8_t *src_start = p;
                while (p < end && *p != '>') p++;
                size_t src_len = (size_t)(p - src_start);
                if (p < end) p++; /* skip > */

                uint32_t glyph_id = parse_cmap_hex(src_start, src_len);

                /* Skip whitespace */
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r'))
                    p++;

                /* Parse <dst> */
                if (p >= end || *p != '<') break;
                p++;
                const uint8_t *dst_start = p;
                while (p < end && *p != '>') p++;
                size_t dst_len = (size_t)(p - dst_start);
                if (p < end) p++; /* skip > */

                uint32_t codepoint = parse_cmap_hex(dst_start, dst_len);

                if (font->tounicode_count < PDF_MAX_TOUNICODE) {
                    font->tounicode[font->tounicode_count].glyph_id = glyph_id;
                    font->tounicode[font->tounicode_count].codepoint = codepoint;
                    font->tounicode_count++;
                }
            }
        } else {
            /* beginbfrange: triples of <srcLo> <srcHi> <dstLo> */
            int limit = 10000;
            while (p < end && limit-- > 0) {
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r'))
                    p++;
                if (p >= end) break;

                if (p + 11 <= end && memcmp(p, "endbfrange", 10) == 0) {
                    p += 10;
                    break;
                }

                /* <srcLo> */
                if (*p != '<') break;
                p++;
                const uint8_t *lo_start = p;
                while (p < end && *p != '>') p++;
                size_t lo_len = (size_t)(p - lo_start);
                if (p < end) p++;

                uint32_t lo = parse_cmap_hex(lo_start, lo_len);

                /* <srcHi> */
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
                if (p >= end || *p != '<') break;
                p++;
                const uint8_t *hi_start = p;
                while (p < end && *p != '>') p++;
                size_t hi_len = (size_t)(p - hi_start);
                if (p < end) p++;

                uint32_t hi = parse_cmap_hex(hi_start, hi_len);

                /* <dstLo> or [...] */
                while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
                if (p >= end) break;

                if (*p == '[') {
                    /* Array of individual mappings */
                    p++; /* skip [ */
                    for (uint32_t gid = lo; gid <= hi && p < end; gid++) {
                        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
                        if (p >= end || *p == ']') break;
                        if (*p != '<') break;
                        p++;
                        const uint8_t *d_start = p;
                        while (p < end && *p != '>') p++;
                        size_t d_len = (size_t)(p - d_start);
                        if (p < end) p++;

                        uint32_t cp = parse_cmap_hex(d_start, d_len);
                        if (font->tounicode_count < PDF_MAX_TOUNICODE) {
                            font->tounicode[font->tounicode_count].glyph_id = gid;
                            font->tounicode[font->tounicode_count].codepoint = cp;
                            font->tounicode_count++;
                        }
                    }
                    while (p < end && *p != ']') p++;
                    if (p < end) p++;
                } else if (*p == '<') {
                    p++;
                    const uint8_t *d_start = p;
                    while (p < end && *p != '>') p++;
                    size_t d_len = (size_t)(p - d_start);
                    if (p < end) p++;

                    uint32_t base = parse_cmap_hex(d_start, d_len);

                    for (uint32_t gid = lo; gid <= hi; gid++) {
                        if (font->tounicode_count < PDF_MAX_TOUNICODE) {
                            font->tounicode[font->tounicode_count].glyph_id = gid;
                            font->tounicode[font->tounicode_count].codepoint =
                                base + (gid - lo);
                            font->tounicode_count++;
                        }
                    }
                }
            }
        }
    }
}

/* Look up glyph ID in ToUnicode CMap */
static uint32_t tounicode_lookup(const PdfFont *font, uint32_t glyph_id)
{
    for (int i = 0; i < font->tounicode_count; i++) {
        if (font->tounicode[i].glyph_id == glyph_id)
            return font->tounicode[i].codepoint;
    }
    return 0xFFFD; /* replacement character */
}

/* Get glyph width from CID /W array */
static double cid_width_lookup(const PdfFont *font, uint32_t cid)
{
    for (int i = 0; i < font->cid_width_count; i++) {
        if (font->cid_widths[i].cid == cid)
            return font->cid_widths[i].width;
    }
    return font->default_width;
}

/* Parse CID /W array: [cid [w1 w2 ...]] or [cidFirst cidLast w] */
static void parse_cid_widths(ShPdf2strucCtx *ctx, PdfFont *font, PdfObj *w_arr)
{
    if (!w_arr || w_arr->type != PDF_OBJ_ARRAY) return;

    int cap = 256;
    font->cid_widths = (PdfCidWidth *)sh_arena_alloc(ctx->arena,
                             (size_t)cap * sizeof(PdfCidWidth));
    if (!font->cid_widths) return;
    font->cid_width_count = 0;

    int n = pdf_array_len(w_arr);
    int i = 0;
    while (i < n) {
        PdfObj *first = pdf_resolve(ctx, pdf_array_get(w_arr, i));
        if (!first || first->type != PDF_OBJ_INT) break;
        int cid_start = (int)first->int_val;
        i++;
        if (i >= n) break;

        PdfObj *next = pdf_resolve(ctx, pdf_array_get(w_arr, i));
        if (!next) break;

        if (next->type == PDF_OBJ_ARRAY) {
            /* [cid [w1 w2 w3 ...]] */
            int wn = pdf_array_len(next);
            for (int j = 0; j < wn; j++) {
                PdfObj *wv = pdf_resolve(ctx, pdf_array_get(next, j));
                double w = wv ? (wv->type == PDF_OBJ_INT ? (double)wv->int_val :
                                 wv->type == PDF_OBJ_REAL ? wv->real_val : 1000.0) : 1000.0;
                if (font->cid_width_count < cap) {
                    font->cid_widths[font->cid_width_count].cid = (uint32_t)(cid_start + j);
                    font->cid_widths[font->cid_width_count].width = w;
                    font->cid_width_count++;
                }
            }
            i++;
        } else if (next->type == PDF_OBJ_INT) {
            /* [cidFirst cidLast w] */
            int cid_last = (int)next->int_val;
            i++;
            if (i >= n) break;
            PdfObj *wv = pdf_resolve(ctx, pdf_array_get(w_arr, i));
            double w = wv ? (wv->type == PDF_OBJ_INT ? (double)wv->int_val :
                             wv->type == PDF_OBJ_REAL ? wv->real_val : 1000.0) : 1000.0;
            for (int c = cid_start; c <= cid_last; c++) {
                if (font->cid_width_count < cap) {
                    font->cid_widths[font->cid_width_count].cid = (uint32_t)c;
                    font->cid_widths[font->cid_width_count].width = w;
                    font->cid_width_count++;
                }
            }
            i++;
        } else {
            break;
        }
    }
}

/* Parse a font resource and add to ctx->fonts */
static int parse_font(ShPdf2strucCtx *ctx, const char *name, PdfObj *font_obj)
{
    if (ctx->font_count >= PDF_MAX_FONTS) return -1;

    font_obj = pdf_resolve(ctx, font_obj);
    if (!font_obj) return -1;
    PdfDict *fd = NULL;
    if (font_obj->type == PDF_OBJ_DICT)
        fd = &font_obj->dict_val;
    else if (font_obj->type == PDF_OBJ_STREAM)
        fd = &font_obj->stream_val.dict;
    else
        return -1;

    /* Check if already parsed */
    for (int i = 0; i < ctx->font_count; i++) {
        if (ctx->fonts[i].name && strcmp(ctx->fonts[i].name, name) == 0)
            return i;
    }

    int idx = ctx->font_count++;
    PdfFont *font = &ctx->fonts[idx];
    font->name = pdf_arena_strndup(ctx, name, strlen(name));
    font->default_width = 1000.0;
    font->ascent = 0.8;
    font->descent = -0.2;

    const char *subtype = pdf_dict_get_name(ctx, fd, "Subtype");
    if (!subtype) subtype = "";

    if (strcmp(subtype, "Type0") == 0) {
        font->type = PDF_FONT_TYPE0;

        /* Parse ToUnicode */
        PdfObj *tu = pdf_resolve(ctx, pdf_dict_get(fd, "ToUnicode"));
        if (tu && tu->type == PDF_OBJ_STREAM) {
            size_t tu_len = 0;
            uint8_t *tu_data = pdf_decompress_stream(ctx, tu, &tu_len);
            if (tu_data && tu_len > 0) {
                parse_tounicode(ctx, font, tu_data, tu_len);
            }
        }

        /* Parse DescendantFonts -> CIDFont -> /W and /DW */
        PdfObj *desc_arr = pdf_resolve(ctx, pdf_dict_get(fd, "DescendantFonts"));
        if (desc_arr && desc_arr->type == PDF_OBJ_ARRAY && pdf_array_len(desc_arr) > 0) {
            PdfObj *cid_font = pdf_resolve(ctx, pdf_array_get(desc_arr, 0));
            if (cid_font) {
                PdfDict *cid_d = NULL;
                if (cid_font->type == PDF_OBJ_DICT) cid_d = &cid_font->dict_val;
                else if (cid_font->type == PDF_OBJ_STREAM) cid_d = &cid_font->stream_val.dict;

                if (cid_d) {
                    font->default_width = pdf_dict_get_real(ctx, cid_d, "DW", 1000.0);
                    PdfObj *w = pdf_resolve(ctx, pdf_dict_get(cid_d, "W"));
                    if (w) parse_cid_widths(ctx, font, w);

                    /* FontDescriptor */
                    PdfObj *fdesc = pdf_resolve(ctx, pdf_dict_get(cid_d, "FontDescriptor"));
                    if (fdesc) {
                        PdfDict *fdd = NULL;
                        if (fdesc->type == PDF_OBJ_DICT) fdd = &fdesc->dict_val;
                        else if (fdesc->type == PDF_OBJ_STREAM) fdd = &fdesc->stream_val.dict;
                        if (fdd) {
                            double asc = pdf_dict_get_real(ctx, fdd, "Ascent", 0);
                            double desc = pdf_dict_get_real(ctx, fdd, "Descent", 0);
                            if (asc != 0) font->ascent = asc / 1000.0;
                            if (desc != 0) font->descent = desc / 1000.0;
                        }
                    }
                }
            }
        }
    } else {
        /* Type1, TrueType, or similar simple font */
        if (strcmp(subtype, "TrueType") == 0)
            font->type = PDF_FONT_TRUETYPE;
        else if (strcmp(subtype, "Type1") == 0)
            font->type = PDF_FONT_TYPE1;
        else if (strcmp(subtype, "Type3") == 0)
            font->type = PDF_FONT_TYPE3;
        else
            font->type = PDF_FONT_TYPE1; /* default */

        /* Parse ToUnicode (some simple fonts have it too) */
        PdfObj *tu = pdf_resolve(ctx, pdf_dict_get(fd, "ToUnicode"));
        if (tu && tu->type == PDF_OBJ_STREAM) {
            size_t tu_len = 0;
            uint8_t *tu_data = pdf_decompress_stream(ctx, tu, &tu_len);
            if (tu_data && tu_len > 0) {
                parse_tounicode(ctx, font, tu_data, tu_len);
            }
        }

        /* /FirstChar, /LastChar, /Widths */
        font->first_char = (int)pdf_dict_get_int(ctx, fd, "FirstChar", 0);
        font->last_char = (int)pdf_dict_get_int(ctx, fd, "LastChar", 255);

        PdfObj *widths = pdf_resolve(ctx, pdf_dict_get(fd, "Widths"));
        if (widths && widths->type == PDF_OBJ_ARRAY) {
            int n = pdf_array_len(widths);
            if (n > 0 && n <= PDF_MAX_WIDTHS) {
                font->widths = (double *)sh_arena_alloc(ctx->arena,
                                    (size_t)n * sizeof(double));
                if (font->widths) {
                    font->widths_count = n;
                    for (int i = 0; i < n; i++) {
                        PdfObj *wv = pdf_resolve(ctx, pdf_array_get(widths, i));
                        font->widths[i] = wv ? (wv->type == PDF_OBJ_INT ?
                            (double)wv->int_val : wv->type == PDF_OBJ_REAL ?
                            wv->real_val : 0) : 0;
                    }
                }
            }
        }

        /* FontDescriptor */
        PdfObj *fdesc = pdf_resolve(ctx, pdf_dict_get(fd, "FontDescriptor"));
        if (fdesc) {
            PdfDict *fdd = NULL;
            if (fdesc->type == PDF_OBJ_DICT) fdd = &fdesc->dict_val;
            else if (fdesc->type == PDF_OBJ_STREAM) fdd = &fdesc->stream_val.dict;
            if (fdd) {
                double asc = pdf_dict_get_real(ctx, fdd, "Ascent", 0);
                double desc = pdf_dict_get_real(ctx, fdd, "Descent", 0);
                if (asc != 0) font->ascent = asc / 1000.0;
                if (desc != 0) font->descent = desc / 1000.0;
            }
        }
    }

    return idx;
}

/* ============================================================================
 * Font Resource Discovery
 * ============================================================================ */

static void parse_page_fonts(ShPdf2strucCtx *ctx, PdfObj *resources)
{
    resources = pdf_resolve(ctx, resources);
    if (!resources) return;

    PdfDict *rd = NULL;
    if (resources->type == PDF_OBJ_DICT) rd = &resources->dict_val;
    else if (resources->type == PDF_OBJ_STREAM) rd = &resources->stream_val.dict;
    if (!rd) return;

    PdfObj *font_dict = pdf_resolve(ctx, pdf_dict_get(rd, "Font"));
    if (!font_dict) return;

    PdfDict *fd = NULL;
    if (font_dict->type == PDF_OBJ_DICT) fd = &font_dict->dict_val;
    else if (font_dict->type == PDF_OBJ_STREAM) fd = &font_dict->stream_val.dict;
    if (!fd) return;

    for (int i = 0; i < fd->count; i++) {
        parse_font(ctx, fd->keys[i], fd->vals[i]);
    }
}

static int find_font_index(ShPdf2strucCtx *ctx, const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < ctx->font_count; i++) {
        if (ctx->fonts[i].name && strcmp(ctx->fonts[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ============================================================================
 * Text Decoding
 * ============================================================================ */

/* Get glyph width for a simple (WinAnsi) font */
static double simple_glyph_width(const PdfFont *font, int charcode)
{
    if (font->widths && font->widths_count > 0) {
        int idx = charcode - font->first_char;
        if (idx >= 0 && idx < font->widths_count)
            return font->widths[idx];
    }
    return 0; /* signals "unknown" */
}

/* Decode a text string into UTF-8 and compute total advance width.
 * Returns arena-allocated UTF-8 string. */
static const char *decode_text_simple(ShPdf2strucCtx *ctx, const PdfFont *font,
                                       const uint8_t *bytes, size_t len,
                                       double font_size, double h_scaling,
                                       double char_space, double word_space,
                                       int approx_widths,
                                       double *out_advance)
{
    /* Worst case: 4 UTF-8 bytes per char; check overflow */
    if (len > (SIZE_MAX - 1) / 4) return NULL;
    char *buf = (char *)sh_arena_alloc(ctx->arena, len * 4 + 1);
    if (!buf) return NULL;

    size_t out = 0;
    double advance = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t code = bytes[i];
        uint32_t cp;

        if (font->has_tounicode) {
            cp = tounicode_lookup(font, code);
        } else {
            cp = WINANSI_TO_UNICODE[code];
            if (cp == 0 && code != 0) cp = code;
        }

        if (cp > 0 && cp != 0xFFFD) {
            out += (size_t)utf8_encode(cp, buf + out);
        }

        /* Advance */
        double w = simple_glyph_width(font, code);
        if (w > 0) {
            advance += (w / 1000.0) * font_size;
        } else if (approx_widths) {
            advance += 0.5 * font_size;
        }

        advance += char_space;
        if (code == 0x20) advance += word_space;
    }

    /* Apply horizontal scaling */
    advance *= h_scaling / 100.0;

    buf[out] = '\0';
    *out_advance = advance;
    return buf;
}

/* Decode a CID/Type0 text string (2-byte glyph IDs from hex string) */
static const char *decode_text_cid(ShPdf2strucCtx *ctx, const PdfFont *font,
                                     const uint8_t *bytes, size_t len,
                                     double font_size, double h_scaling,
                                     double char_space, double word_space,
                                     int approx_widths,
                                     double *out_advance)
{
    /* 2 bytes per glyph, 4 UTF-8 bytes per char max; check overflow */
    size_t max_chars = len / 2;
    if (max_chars > (SIZE_MAX - 1) / 4) return NULL;
    char *buf = (char *)sh_arena_alloc(ctx->arena, max_chars * 4 + 1);
    if (!buf) return NULL;

    size_t out = 0;
    double advance = 0;

    for (size_t i = 0; i + 1 < len; i += 2) {
        uint32_t gid = ((uint32_t)bytes[i] << 8) | bytes[i + 1];

        uint32_t cp;
        if (font->has_tounicode) {
            cp = tounicode_lookup(font, gid);
        } else {
            cp = gid; /* Identity mapping fallback */
        }

        if (cp > 0 && cp != 0xFFFD) {
            out += (size_t)utf8_encode(cp, buf + out);
        }

        double w = cid_width_lookup(font, gid);
        if (w > 0) {
            advance += (w / 1000.0) * font_size;
        } else if (approx_widths) {
            advance += 0.5 * font_size;
        }

        advance += char_space;
        if (cp == 0x0020) advance += word_space;
    }

    advance *= h_scaling / 100.0;

    buf[out] = '\0';
    *out_advance = advance;
    return buf;
}

/* ============================================================================
 * Add Text Run
 * ============================================================================ */

static void add_run(ShPdf2strucCtx *ctx, int page_idx,
                     const char *text, double advance,
                     const PdfMatrix *tm, const PdfMatrix *ctm,
                     double font_size, double h_scaling, double rise,
                     const PdfFont *font)
{
    if (!text || text[0] == '\0') return;
    if (ctx->run_count >= PDF_MAX_RUNS) return;

    /* Expand if needed */
    if (ctx->run_count >= ctx->run_capacity) {
        int new_cap = ctx->run_capacity * 2;
        if (new_cap > PDF_MAX_RUNS) new_cap = PDF_MAX_RUNS;
        PdfTextRun *new_runs = (PdfTextRun *)sh_arena_alloc(ctx->arena,
                                    (size_t)new_cap * sizeof(PdfTextRun));
        if (!new_runs) return;
        memcpy(new_runs, ctx->runs,
               (size_t)ctx->run_count * sizeof(PdfTextRun));
        ctx->runs = new_runs;
        ctx->run_capacity = new_cap;
    }

    /* Compute text rendering matrix: S * Tm * CTM */
    double fs = font_size;
    double hs = h_scaling / 100.0;
    PdfMatrix s_mat = { fs * hs, 0, 0, fs, 0, rise * fs };
    PdfMatrix trm = mat_mul(s_mat, mat_mul(*tm, *ctm));

    /* Bbox in glyph space: (0, descent) to (advance, ascent) */
    double asc = font ? font->ascent : 0.8;
    double desc = font ? font->descent : -0.2;

    /* Transform 4 corners to get axis-aligned bbox */
    double gx[4], gy[4];
    /* Normalize advance to glyph space units: we already have advance in
       user space incorporating font_size and h_scaling, but TRM already
       includes those. So we need advance in pre-TRM units. */
    double norm_advance = (fs * hs > 0) ? advance / (fs * hs) : 0;

    /* Corners in font matrix space (before TRM) */
    double cx[4] = { 0, norm_advance, norm_advance, 0 };
    double cy[4] = { desc, desc, asc, asc };

    for (int i = 0; i < 4; i++)
        mat_apply(&trm, cx[i], cy[i], &gx[i], &gy[i]);

    double min_x = gx[0], max_x = gx[0];
    double min_y = gy[0], max_y = gy[0];
    for (int i = 1; i < 4; i++) {
        if (gx[i] < min_x) min_x = gx[i];
        if (gx[i] > max_x) max_x = gx[i];
        if (gy[i] < min_y) min_y = gy[i];
        if (gy[i] > max_y) max_y = gy[i];
    }

    PdfTextRun *run = &ctx->runs[ctx->run_count++];
    run->page_index = page_idx;
    run->x = min_x;
    run->y = min_y;
    run->w = max_x - min_x;
    run->h = max_y - min_y;
    run->text = text;
}

/* ============================================================================
 * Content Stream Operator Dispatch
 * ============================================================================ */

typedef struct {
    PdfObj *stack[PDF_MAX_OPERAND_STACK];
    int     top;
} OperandStack;

static void op_push(OperandStack *ops, PdfObj *obj)
{
    if (ops->top < PDF_MAX_OPERAND_STACK)
        ops->stack[ops->top++] = obj;
}

static PdfObj *op_pop(OperandStack *ops)
{
    if (ops->top <= 0) return NULL;
    return ops->stack[--ops->top];
}

static double op_pop_num(OperandStack *ops, double def)
{
    PdfObj *o = op_pop(ops);
    if (!o) return def;
    if (o->type == PDF_OBJ_INT) return (double)o->int_val;
    if (o->type == PDF_OBJ_REAL) return o->real_val;
    return def;
}

static void process_content_stream(ShPdf2strucCtx *ctx, int page_idx,
                                     const uint8_t *data, size_t len,
                                     const ShPdf2strucOpts *opt)
{
    /* Graphics state stack */
    PdfGState gstack[PDF_MAX_GSTATE_DEPTH];
    int gstack_top = 0;

    PdfGState gs;
    gs.ctm = mat_identity();
    gs.font_idx = -1;
    gs.font_size = 12;
    gs.char_space = 0;
    gs.word_space = 0;
    gs.h_scaling = 100;
    gs.leading = 0;
    gs.rise = 0;

    /* Text state */
    PdfMatrix tm = mat_identity();
    PdfMatrix tlm = mat_identity(); /* text line matrix */
    int in_text = 0;

    /* Operand stack */
    OperandStack ops;
    ops.top = 0;

    /* Parse content stream with simple tokenizer */
    PdfScanner s;
    s.data = data;
    s.size = len;
    s.pos = 0;

    int op_count = 0;
    int max_ops = PDF_MAX_CONTENT_OPS;

    while (s.pos < s.size && op_count < max_ops) {
        /* Skip whitespace */
        while (s.pos < s.size) {
            uint8_t c = s.data[s.pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                c == '\f' || c == '\0') {
                s.pos++;
            } else if (c == '%') {
                while (s.pos < s.size && s.data[s.pos] != '\n' &&
                       s.data[s.pos] != '\r')
                    s.pos++;
            } else {
                break;
            }
        }
        if (s.pos >= s.size) break;

        uint8_t c = s.data[s.pos];

        /* Operands: numbers, strings, names, arrays, dicts */
        if (c == '(' || c == '<' || c == '/' || c == '[' ||
            c == '+' || c == '-' || c == '.' ||
            (c >= '0' && c <= '9')) {

            PdfObj *obj = pdf_parse_obj(ctx, &s);
            if (obj) op_push(&ops, obj);
            continue;
        }

        /* Operator: alphabetic keyword */
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '\'' || c == '"' || c == '*') {
            size_t op_start = s.pos;
            while (s.pos < s.size) {
                uint8_t oc = s.data[s.pos];
                if ((oc >= 'a' && oc <= 'z') || (oc >= 'A' && oc <= 'Z') ||
                    oc == '*' || oc == '\'' || oc == '"' ||
                    (oc >= '0' && oc <= '9'))
                    s.pos++;
                else
                    break;
            }
            size_t op_len = s.pos - op_start;
            op_count++;

            /* Match operators */
            const uint8_t *op = data + op_start;

            #define OP_IS(str) (op_len == strlen(str) && memcmp(op, str, op_len) == 0)

            if (OP_IS("BT")) {
                in_text = 1;
                tm = mat_identity();
                tlm = mat_identity();
            }
            else if (OP_IS("ET")) {
                in_text = 0;
            }
            else if (OP_IS("q")) {
                if (gstack_top < PDF_MAX_GSTATE_DEPTH)
                    gstack[gstack_top++] = gs;
            }
            else if (OP_IS("Q")) {
                if (gstack_top > 0)
                    gs = gstack[--gstack_top];
            }
            else if (OP_IS("cm")) {
                double f = op_pop_num(&ops, 0);
                double e = op_pop_num(&ops, 0);
                double d = op_pop_num(&ops, 1);
                double cc = op_pop_num(&ops, 0);
                double b = op_pop_num(&ops, 0);
                double a = op_pop_num(&ops, 1);
                PdfMatrix m = {a, b, cc, d, e, f};
                gs.ctm = mat_mul(m, gs.ctm);
            }
            else if (OP_IS("Tf") && in_text) {
                double size = op_pop_num(&ops, 12);
                PdfObj *name_obj = op_pop(&ops);
                if (name_obj && name_obj->type == PDF_OBJ_NAME) {
                    int fi = find_font_index(ctx, name_obj->name_val);
                    if (fi >= 0) gs.font_idx = fi;
                }
                gs.font_size = size;
            }
            else if (OP_IS("Tm") && in_text) {
                double f = op_pop_num(&ops, 0);
                double e = op_pop_num(&ops, 0);
                double d = op_pop_num(&ops, 1);
                double cc = op_pop_num(&ops, 0);
                double b = op_pop_num(&ops, 0);
                double a = op_pop_num(&ops, 1);
                tm = (PdfMatrix){a, b, cc, d, e, f};
                tlm = tm;
            }
            else if (OP_IS("Td") && in_text) {
                double ty = op_pop_num(&ops, 0);
                double tx = op_pop_num(&ops, 0);
                PdfMatrix t = {1, 0, 0, 1, tx, ty};
                tlm = mat_mul(t, tlm);
                tm = tlm;
            }
            else if (OP_IS("TD") && in_text) {
                double ty = op_pop_num(&ops, 0);
                double tx = op_pop_num(&ops, 0);
                gs.leading = -ty;
                PdfMatrix t = {1, 0, 0, 1, tx, ty};
                tlm = mat_mul(t, tlm);
                tm = tlm;
            }
            else if (OP_IS("T*") && in_text) {
                PdfMatrix t = {1, 0, 0, 1, 0, -gs.leading};
                tlm = mat_mul(t, tlm);
                tm = tlm;
            }
            else if (OP_IS("Tc")) {
                gs.char_space = op_pop_num(&ops, 0);
            }
            else if (OP_IS("Tw")) {
                gs.word_space = op_pop_num(&ops, 0);
            }
            else if (OP_IS("Tz")) {
                gs.h_scaling = op_pop_num(&ops, 100);
            }
            else if (OP_IS("TL")) {
                gs.leading = op_pop_num(&ops, 0);
            }
            else if (OP_IS("Ts")) {
                gs.rise = op_pop_num(&ops, 0);
            }
            else if (OP_IS("Tj") && in_text) {
                PdfObj *str = op_pop(&ops);
                if (str && (str->type == PDF_OBJ_STRING ||
                            str->type == PDF_OBJ_HEXSTRING)) {
                    const PdfFont *font = (gs.font_idx >= 0 &&
                        gs.font_idx < ctx->font_count) ?
                        &ctx->fonts[gs.font_idx] : NULL;

                    double advance = 0;
                    const char *text;
                    if (font && font->type == PDF_FONT_TYPE0)
                        text = decode_text_cid(ctx, font,
                            str->string_val.data, str->string_val.len,
                            gs.font_size, gs.h_scaling,
                            gs.char_space, gs.word_space,
                            opt->approx_widths, &advance);
                    else
                        text = decode_text_simple(ctx, font,
                            str->string_val.data, str->string_val.len,
                            gs.font_size, gs.h_scaling,
                            gs.char_space, gs.word_space,
                            opt->approx_widths, &advance);

                    if (text) {
                        add_run(ctx, page_idx, text, advance,
                                &tm, &gs.ctm, gs.font_size,
                                gs.h_scaling, gs.rise, font);
                    }

                    /* Advance text position */
                    double tx_advance = advance / (gs.font_size * gs.h_scaling / 100.0);
                    if (gs.font_size * gs.h_scaling != 0) {
                        PdfMatrix adv = {1, 0, 0, 1, tx_advance, 0};
                        tm = mat_mul(adv, tm);
                    }
                }
            }
            else if (OP_IS("TJ") && in_text) {
                PdfObj *arr = op_pop(&ops);
                if (arr && arr->type == PDF_OBJ_ARRAY) {
                    const PdfFont *font = (gs.font_idx >= 0 &&
                        gs.font_idx < ctx->font_count) ?
                        &ctx->fonts[gs.font_idx] : NULL;

                    int n = pdf_array_len(arr);
                    for (int i = 0; i < n; i++) {
                        PdfObj *item = pdf_array_get(arr, i);
                        if (!item) continue;

                        if (item->type == PDF_OBJ_STRING ||
                            item->type == PDF_OBJ_HEXSTRING) {
                            double advance = 0;
                            const char *text;
                            if (font && font->type == PDF_FONT_TYPE0)
                                text = decode_text_cid(ctx, font,
                                    item->string_val.data,
                                    item->string_val.len,
                                    gs.font_size, gs.h_scaling,
                                    gs.char_space, gs.word_space,
                                    opt->approx_widths, &advance);
                            else
                                text = decode_text_simple(ctx, font,
                                    item->string_val.data,
                                    item->string_val.len,
                                    gs.font_size, gs.h_scaling,
                                    gs.char_space, gs.word_space,
                                    opt->approx_widths, &advance);

                            if (text) {
                                add_run(ctx, page_idx, text, advance,
                                        &tm, &gs.ctm, gs.font_size,
                                        gs.h_scaling, gs.rise, font);
                            }

                            double tx_advance = (gs.font_size * gs.h_scaling / 100.0 != 0) ?
                                advance / (gs.font_size * gs.h_scaling / 100.0) : 0;
                            PdfMatrix adv = {1, 0, 0, 1, tx_advance, 0};
                            tm = mat_mul(adv, tm);
                        }
                        else if (item->type == PDF_OBJ_INT ||
                                 item->type == PDF_OBJ_REAL) {
                            /* Kerning adjustment: negative = move right */
                            double adj = (item->type == PDF_OBJ_INT) ?
                                (double)item->int_val : item->real_val;
                            double tx = -adj / 1000.0;
                            PdfMatrix adv = {1, 0, 0, 1, tx, 0};
                            tm = mat_mul(adv, tm);
                        }
                    }
                }
            }
            else if ((OP_IS("'")) && in_text) {
                /* ' = T* + Tj */
                PdfObj *str = op_pop(&ops);
                PdfMatrix t = {1, 0, 0, 1, 0, -gs.leading};
                tlm = mat_mul(t, tlm);
                tm = tlm;
                if (str) {
                    op_push(&ops, str);
                    /* Reparse as Tj - inline the logic */
                    /* Just re-push and handle via goto would be complex,
                       so duplicate the Tj logic here: */
                    str = op_pop(&ops);
                    if (str && (str->type == PDF_OBJ_STRING ||
                                str->type == PDF_OBJ_HEXSTRING)) {
                        const PdfFont *font = (gs.font_idx >= 0 &&
                            gs.font_idx < ctx->font_count) ?
                            &ctx->fonts[gs.font_idx] : NULL;
                        double advance = 0;
                        const char *text;
                        if (font && font->type == PDF_FONT_TYPE0)
                            text = decode_text_cid(ctx, font,
                                str->string_val.data, str->string_val.len,
                                gs.font_size, gs.h_scaling,
                                gs.char_space, gs.word_space,
                                opt->approx_widths, &advance);
                        else
                            text = decode_text_simple(ctx, font,
                                str->string_val.data, str->string_val.len,
                                gs.font_size, gs.h_scaling,
                                gs.char_space, gs.word_space,
                                opt->approx_widths, &advance);
                        if (text) {
                            add_run(ctx, page_idx, text, advance,
                                    &tm, &gs.ctm, gs.font_size,
                                    gs.h_scaling, gs.rise, font);
                        }
                        double tx_advance = (gs.font_size * gs.h_scaling / 100.0 != 0) ?
                            advance / (gs.font_size * gs.h_scaling / 100.0) : 0;
                        PdfMatrix adv = {1, 0, 0, 1, tx_advance, 0};
                        tm = mat_mul(adv, tm);
                    }
                }
            }
            else if (OP_IS("\"") && in_text) {
                /* " = Tw + Tc + T* + Tj */
                PdfObj *str = op_pop(&ops);
                gs.char_space = op_pop_num(&ops, 0);
                gs.word_space = op_pop_num(&ops, 0);
                PdfMatrix t = {1, 0, 0, 1, 0, -gs.leading};
                tlm = mat_mul(t, tlm);
                tm = tlm;
                if (str && (str->type == PDF_OBJ_STRING ||
                            str->type == PDF_OBJ_HEXSTRING)) {
                    const PdfFont *font = (gs.font_idx >= 0 &&
                        gs.font_idx < ctx->font_count) ?
                        &ctx->fonts[gs.font_idx] : NULL;
                    double advance = 0;
                    const char *text;
                    if (font && font->type == PDF_FONT_TYPE0)
                        text = decode_text_cid(ctx, font,
                            str->string_val.data, str->string_val.len,
                            gs.font_size, gs.h_scaling,
                            gs.char_space, gs.word_space,
                            opt->approx_widths, &advance);
                    else
                        text = decode_text_simple(ctx, font,
                            str->string_val.data, str->string_val.len,
                            gs.font_size, gs.h_scaling,
                            gs.char_space, gs.word_space,
                            opt->approx_widths, &advance);
                    if (text) {
                        add_run(ctx, page_idx, text, advance,
                                &tm, &gs.ctm, gs.font_size,
                                gs.h_scaling, gs.rise, font);
                    }
                    double tx_advance = (gs.font_size * gs.h_scaling / 100.0 != 0) ?
                        advance / (gs.font_size * gs.h_scaling / 100.0) : 0;
                    PdfMatrix adv = {1, 0, 0, 1, tx_advance, 0};
                    tm = mat_mul(adv, tm);
                }
            }
            /* All other operators: just consume and clear the operand stack */
            else {
                ops.top = 0;
            }

            #undef OP_IS
            continue;
        }

        /* Unknown byte - skip */
        s.pos++;
    }
}

/* ============================================================================
 * Page Content Extraction
 * ============================================================================ */

ShPdf2strucStatus pdf_extract_text(ShPdf2strucCtx *ctx, const ShPdf2strucOpts *opt)
{
    for (int p = 0; p < ctx->page_count; p++) {
        PdfPageInfo *pi = &ctx->pages[p];

        /* Parse fonts for this page */
        parse_page_fonts(ctx, pi->resources);

        /* Get content stream(s) */
        PdfObj *contents = pdf_resolve(ctx, pi->contents);
        if (!contents) continue;

        if (contents->type == PDF_OBJ_STREAM) {
            size_t decomp_len = 0;
            uint8_t *decomp = pdf_decompress_stream(ctx, contents, &decomp_len);
            if (decomp && decomp_len > 0) {
                process_content_stream(ctx, p, decomp, decomp_len, opt);
            }
        }
        else if (contents->type == PDF_OBJ_ARRAY) {
            int n = pdf_array_len(contents);
            for (int i = 0; i < n; i++) {
                PdfObj *stream = pdf_resolve(ctx, pdf_array_get(contents, i));
                if (stream && stream->type == PDF_OBJ_STREAM) {
                    size_t decomp_len = 0;
                    uint8_t *decomp = pdf_decompress_stream(ctx, stream, &decomp_len);
                    if (decomp && decomp_len > 0) {
                        process_content_stream(ctx, p, decomp, decomp_len, opt);
                    }
                }
            }
        }
        /* Ref: resolve and recurse */
        else if (contents->type == PDF_OBJ_REF) {
            PdfObj *resolved = pdf_resolve(ctx, contents);
            if (resolved && resolved->type == PDF_OBJ_STREAM) {
                size_t decomp_len = 0;
                uint8_t *decomp = pdf_decompress_stream(ctx, resolved, &decomp_len);
                if (decomp && decomp_len > 0) {
                    process_content_stream(ctx, p, decomp, decomp_len, opt);
                }
            }
        }
    }

    return SH_PDF2STRUC_OK;
}

/* ============================================================================
 * Run Grouping into Word-Level Blocks
 * ============================================================================ */

static int run_compare(const void *a, const void *b)
{
    const PdfTextRun *ra = (const PdfTextRun *)a;
    const PdfTextRun *rb = (const PdfTextRun *)b;

    if (ra->page_index != rb->page_index)
        return ra->page_index - rb->page_index;

    /* Sort by y (top to bottom), then x (left to right) */
    double dy = ra->y - rb->y;
    if (dy < -0.5) return -1;
    if (dy > 0.5) return 1;

    double dx = ra->x - rb->x;
    if (dx < -0.01) return -1;
    if (dx > 0.01) return 1;
    return 0;
}

ShPdf2strucStatus pdf_group_runs(ShPdf2strucCtx *ctx, const ShPdf2strucOpts *opt,
                                   ShPdf2strucCallback cb, void *user)
{
    if (ctx->run_count == 0) return SH_PDF2STRUC_OK;
    if (!cb) return SH_PDF2STRUC_OK;

    /* Arena-allocate page heights (PDF_MAX_PAGES * 8 bytes could be ~80KB) */
    double *page_heights = (double *)sh_arena_alloc(ctx->arena,
                                (size_t)ctx->page_count * sizeof(double));
    if (!page_heights) return SH_PDF2STRUC_ERR_OOM;
    for (int i = 0; i < ctx->page_count; i++) {
        page_heights[i] = ctx->pages[i].height;
    }

    /* Sort runs */
    qsort(ctx->runs, (size_t)ctx->run_count, sizeof(PdfTextRun), run_compare);

    if (opt->emit_mode == SH_PDF2STRUC_EMIT_RUNS) {
        /* Emit each run as a separate block */
        for (int i = 0; i < ctx->run_count; i++) {
            PdfTextRun *r = &ctx->runs[i];
            ShPdf2strucBlock blk;
            blk.page_index = r->page_index;
            blk.x = r->x;
            blk.y = opt->origin_top_left && r->page_index < ctx->page_count ?
                     page_heights[r->page_index] - r->y - r->h : r->y;
            blk.w = r->w;
            blk.h = r->h;
            blk.text = r->text;
            cb(user, &blk);
        }
        return SH_PDF2STRUC_OK;
    }

    /* EMIT_BLOCKS mode: merge adjacent runs into word-level blocks */
    double y_eps = opt->merge_y_epsilon;
    double x_gap = opt->merge_x_gap;

    int i = 0;
    while (i < ctx->run_count) {
        PdfTextRun *first = &ctx->runs[i];

        /* Collect runs on same line */
        int j = i + 1;
        while (j < ctx->run_count &&
               ctx->runs[j].page_index == first->page_index &&
               fabs(ctx->runs[j].y - first->y) < y_eps) {
            j++;
        }

        /* Process runs [i, j) on same line */
        int k = i;
        while (k < j) {
            PdfTextRun *start = &ctx->runs[k];
            double merged_x = start->x;
            double merged_y = start->y;
            double merged_h = start->h;
            double merged_right = start->x + start->w;

            /* Merge text */
            size_t text_cap = 256;
            size_t text_len = 0;
            char *text_buf = (char *)sh_arena_alloc(ctx->arena, text_cap);
            if (!text_buf) break;

            size_t slen = strlen(start->text);
            if (slen > text_cap - 1) slen = text_cap - 1;
            memcpy(text_buf, start->text, slen);
            text_len = slen;

            int m = k + 1;
            while (m < j) {
                PdfTextRun *next = &ctx->runs[m];
                double gap = next->x - merged_right;
                if (gap > x_gap) break;

                /* Append with space if there's a gap, otherwise concatenate */
                size_t nlen = strlen(next->text);
                size_t needed = text_len + nlen + 2;
                if (needed > text_cap) {
                    size_t new_cap = text_cap * 2;
                    if (new_cap < needed) new_cap = needed;
                    char *new_buf = (char *)sh_arena_alloc(ctx->arena, new_cap);
                    if (!new_buf) break;
                    memcpy(new_buf, text_buf, text_len);
                    text_buf = new_buf;
                    text_cap = new_cap;
                }

                text_buf[text_len] = '\0';
                memcpy(text_buf + text_len, next->text, nlen);
                text_len += nlen;

                merged_right = next->x + next->w;
                if (next->h > merged_h) merged_h = next->h;

                m++;
            }
            text_buf[text_len] = '\0';

            /* Emit block */
            ShPdf2strucBlock blk;
            blk.page_index = first->page_index;
            blk.x = merged_x;
            blk.y = opt->origin_top_left &&
                     first->page_index < ctx->page_count ?
                     page_heights[first->page_index] - merged_y - merged_h :
                     merged_y;
            blk.w = merged_right - merged_x;
            blk.h = merged_h;
            blk.text = text_buf;

            /* Only emit non-empty blocks */
            if (text_len > 0) {
                /* Trim whitespace */
                while (text_len > 0 && (text_buf[text_len - 1] == ' ' ||
                       text_buf[text_len - 1] == '\t'))
                    text_buf[--text_len] = '\0';
                const char *trimmed = text_buf;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                if (*trimmed) {
                    blk.text = trimmed;
                    cb(user, &blk);
                }
            }

            k = m;
        }

        i = j;
    }

    return SH_PDF2STRUC_OK;
}
