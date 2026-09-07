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

#include "sh_mem.h"
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

/* Parse hex-encoded multi-codepoint sequence into UTF-8 string.
 * CMap values >4 hex digits represent multiple 16-bit codepoints (e.g. ligatures).
 * Returns the primary (first) codepoint. */
static uint32_t parse_cmap_hex_to_utf8(const uint8_t *data, size_t len, char *out)
{
    /* Count actual hex digits */
    size_t ndigits = 0;
    for (size_t i = 0; i < len; i++) {
        if (hex_val(data[i]) >= 0) ndigits++;
    }

    /* Single codepoint: up to 4 hex digits (16-bit) */
    if (ndigits <= 4) {
        uint32_t cp = parse_cmap_hex(data, len);
        int n = utf8_encode(cp, out);
        out[n] = '\0';
        return cp;
    }

    /* Multi-codepoint: pairs of 4 hex digits = 16-bit codepoints each */
    size_t out_pos = 0;
    uint32_t first_cp = 0;
    uint32_t cur = 0;
    int digit_count = 0;

    for (size_t i = 0; i < len && out_pos < 14; i++) {
        int d = hex_val(data[i]);
        if (d < 0) continue;
        cur = (cur << 4) | (uint32_t)d;
        digit_count++;
        if (digit_count == 4) {
            if (first_cp == 0) first_cp = cur;
            int n = utf8_encode(cur, out + out_pos);
            out_pos += (size_t)n;
            cur = 0;
            digit_count = 0;
        }
    }
    /* Handle remaining digits (shouldn't happen for well-formed CMaps) */
    if (digit_count > 0 && out_pos < 14) {
        if (first_cp == 0) first_cp = cur;
        int n = utf8_encode(cur, out + out_pos);
        out_pos += (size_t)n;
    }
    out[out_pos] = '\0';
    return first_cp;
}

/* Comparator for sorting ToUnicode entries by glyph_id */
static int tounicode_compare(const void *a, const void *b)
{
    const PdfToUnicodeEntry *ea = (const PdfToUnicodeEntry *)a;
    const PdfToUnicodeEntry *eb = (const PdfToUnicodeEntry *)b;
    if (ea->glyph_id < eb->glyph_id) return -1;
    if (ea->glyph_id > eb->glyph_id) return 1;
    return 0;
}

/* Comparator for sorting CID widths by cid */
static int cid_width_compare(const void *a, const void *b)
{
    const PdfCidWidth *wa = (const PdfCidWidth *)a;
    const PdfCidWidth *wb = (const PdfCidWidth *)b;
    if (wa->cid < wb->cid) return -1;
    if (wa->cid > wb->cid) return 1;
    return 0;
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
        const uint8_t *bfchar = (const uint8_t *)sh_memmem(
            p, (size_t)(end - p), "beginbfchar", 11);
        const uint8_t *bfrange = (const uint8_t *)sh_memmem(
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

                if (font->tounicode_count < PDF_MAX_TOUNICODE) {
                    PdfToUnicodeEntry *e = &font->tounicode[font->tounicode_count];
                    e->glyph_id = glyph_id;
                    e->codepoint = parse_cmap_hex_to_utf8(dst_start, dst_len, e->text);
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

                        if (font->tounicode_count < PDF_MAX_TOUNICODE) {
                            PdfToUnicodeEntry *e = &font->tounicode[font->tounicode_count];
                            e->glyph_id = gid;
                            e->codepoint = parse_cmap_hex_to_utf8(d_start, d_len, e->text);
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
                            PdfToUnicodeEntry *e = &font->tounicode[font->tounicode_count];
                            e->glyph_id = gid;
                            e->codepoint = base + (gid - lo);
                            int n = utf8_encode(e->codepoint, e->text);
                            e->text[n] = '\0';
                            font->tounicode_count++;
                        }
                    }
                }
            }
        }
    }

    /* Sort entries by glyph_id for binary search */
    if (font->tounicode_count > 1) {
        qsort(font->tounicode, (size_t)font->tounicode_count,
              sizeof(PdfToUnicodeEntry), tounicode_compare);
    }
}

/* Look up glyph ID in ToUnicode CMap (binary search) */
static const PdfToUnicodeEntry *tounicode_find(const PdfFont *font, uint32_t glyph_id)
{
    int lo = 0, hi = font->tounicode_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (font->tounicode[mid].glyph_id == glyph_id)
            return &font->tounicode[mid];
        if (font->tounicode[mid].glyph_id < glyph_id) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* Get glyph width from CID /W array (binary search) */
static double cid_width_lookup(const PdfFont *font, uint32_t cid)
{
    int lo = 0, hi = font->cid_width_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (font->cid_widths[mid].cid == cid) return font->cid_widths[mid].width;
        if (font->cid_widths[mid].cid < cid) lo = mid + 1;
        else hi = mid - 1;
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

    /* Sort by CID for binary search */
    if (font->cid_width_count > 1) {
        qsort(font->cid_widths, (size_t)font->cid_width_count,
              sizeof(PdfCidWidth), cid_width_compare);
    }
}

/* ============================================================================
 * Encoding Tables (MacRoman, Standard)
 * ============================================================================ */

static const uint16_t MACROMAN_TO_UNICODE[256] = {
    /* 0x00-0x7F: identical to ASCII */
    0x0000,0x0001,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,
    0x0008,0x0009,0x000A,0x000B,0x000C,0x000D,0x000E,0x000F,
    0x0010,0x0011,0x0012,0x0013,0x0014,0x0015,0x0016,0x0017,
    0x0018,0x0019,0x001A,0x001B,0x001C,0x001D,0x001E,0x001F,
    0x0020,0x0021,0x0022,0x0023,0x0024,0x0025,0x0026,0x0027,
    0x0028,0x0029,0x002A,0x002B,0x002C,0x002D,0x002E,0x002F,
    0x0030,0x0031,0x0032,0x0033,0x0034,0x0035,0x0036,0x0037,
    0x0038,0x0039,0x003A,0x003B,0x003C,0x003D,0x003E,0x003F,
    0x0040,0x0041,0x0042,0x0043,0x0044,0x0045,0x0046,0x0047,
    0x0048,0x0049,0x004A,0x004B,0x004C,0x004D,0x004E,0x004F,
    0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,0x0056,0x0057,
    0x0058,0x0059,0x005A,0x005B,0x005C,0x005D,0x005E,0x005F,
    0x0060,0x0061,0x0062,0x0063,0x0064,0x0065,0x0066,0x0067,
    0x0068,0x0069,0x006A,0x006B,0x006C,0x006D,0x006E,0x006F,
    0x0070,0x0071,0x0072,0x0073,0x0074,0x0075,0x0076,0x0077,
    0x0078,0x0079,0x007A,0x007B,0x007C,0x007D,0x007E,0x007F,
    /* 0x80-0xFF: MacRoman high range */
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1,
    0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8,
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3,
    0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC,
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF,
    0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8,
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211,
    0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8,
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB,
    0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153,
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA,
    0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02,
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1,
    0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4,
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC,
    0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7,
};

/* Adobe glyph name → Unicode (common entries for /Differences parsing) */
typedef struct { const char *name; uint16_t cp; } AdobeGlyphEntry;

static const AdobeGlyphEntry ADOBE_GLYPH_TABLE[] = {
    {"A",0x0041},{"AE",0x00C6},{"Aacute",0x00C1},{"Acircumflex",0x00C2},
    {"Adieresis",0x00C4},{"Agrave",0x00C0},{"Aring",0x00C5},{"Atilde",0x00C3},
    {"B",0x0042},{"C",0x0043},{"Ccedilla",0x00C7},{"D",0x0044},
    {"E",0x0045},{"Eacute",0x00C9},{"Ecircumflex",0x00CA},
    {"Edieresis",0x00CB},{"Egrave",0x00C8},{"Eth",0x00D0},{"Euro",0x20AC},
    {"F",0x0046},{"G",0x0047},{"H",0x0048},{"I",0x0049},
    {"Iacute",0x00CD},{"Icircumflex",0x00CE},{"Idieresis",0x00CF},
    {"Igrave",0x00CC},{"J",0x004A},{"K",0x004B},{"L",0x004C},
    {"M",0x004D},{"N",0x004E},{"Ntilde",0x00D1},{"O",0x004F},
    {"OE",0x0152},{"Oacute",0x00D3},{"Ocircumflex",0x00D4},
    {"Odieresis",0x00D6},{"Ograve",0x00D2},{"Oslash",0x00D8},
    {"Otilde",0x00D5},{"P",0x0050},{"Q",0x0051},{"R",0x0052},
    {"S",0x0053},{"Scaron",0x0160},{"T",0x0054},{"Thorn",0x00DE},
    {"U",0x0055},{"Uacute",0x00DA},{"Ucircumflex",0x00DB},
    {"Udieresis",0x00DC},{"Ugrave",0x00D9},{"V",0x0056},{"W",0x0057},
    {"X",0x0058},{"Y",0x0059},{"Yacute",0x00DD},{"Ydieresis",0x0178},
    {"Z",0x005A},{"Zcaron",0x017D},
    {"a",0x0061},{"aacute",0x00E1},{"acircumflex",0x00E2},
    {"acute",0x00B4},{"adieresis",0x00E4},{"ae",0x00E6},{"agrave",0x00E0},
    {"ampersand",0x0026},{"aring",0x00E5},{"asciicircum",0x005E},
    {"asciitilde",0x007E},{"asterisk",0x002A},{"at",0x0040},
    {"atilde",0x00E3},{"b",0x0062},{"backslash",0x005C},{"bar",0x007C},
    {"braceleft",0x007B},{"braceright",0x007D},{"bracketleft",0x005B},
    {"bracketright",0x005D},{"breve",0x02D8},{"brokenbar",0x00A6},
    {"bullet",0x2022},{"c",0x0063},{"caron",0x02C7},{"ccedilla",0x00E7},
    {"cedilla",0x00B8},{"cent",0x00A2},{"circumflex",0x02C6},
    {"colon",0x003A},{"comma",0x002C},{"copyright",0x00A9},
    {"currency",0x00A4},{"d",0x0064},{"dagger",0x2020},
    {"daggerdbl",0x2021},{"degree",0x00B0},{"dieresis",0x00A8},
    {"divide",0x00F7},{"dollar",0x0024},{"dotaccent",0x02D9},
    {"dotlessi",0x0131},{"e",0x0065},{"eacute",0x00E9},
    {"ecircumflex",0x00EA},{"edieresis",0x00EB},{"egrave",0x00E8},
    {"eight",0x0038},{"ellipsis",0x2026},{"emdash",0x2014},
    {"endash",0x2013},{"equal",0x003D},{"eth",0x00F0},
    {"exclam",0x0021},{"exclamdown",0x00A1},{"f",0x0066},
    {"fi",0xFB01},{"five",0x0035},{"fl",0xFB02},{"florin",0x0192},
    {"four",0x0034},{"fraction",0x2044},{"g",0x0067},
    {"germandbls",0x00DF},{"grave",0x0060},{"greater",0x003E},
    {"guillemotleft",0x00AB},{"guillemotright",0x00BB},
    {"guilsinglleft",0x2039},{"guilsinglright",0x203A},{"h",0x0068},
    {"hungarumlaut",0x02DD},{"hyphen",0x002D},{"i",0x0069},
    {"iacute",0x00ED},{"icircumflex",0x00EE},{"idieresis",0x00EF},
    {"igrave",0x00EC},{"j",0x006A},{"k",0x006B},{"l",0x006C},
    {"less",0x003C},{"logicalnot",0x00AC},{"lslash",0x0142},
    {"m",0x006D},{"macron",0x00AF},{"minus",0x2212},{"mu",0x00B5},
    {"multiply",0x00D7},{"n",0x006E},{"nine",0x0039},{"ntilde",0x00F1},
    {"numbersign",0x0023},{"o",0x006F},{"oacute",0x00F3},
    {"ocircumflex",0x00F4},{"odieresis",0x00F6},{"oe",0x0153},
    {"ogonek",0x02DB},{"ograve",0x00F2},{"one",0x0031},
    {"onehalf",0x00BD},{"onequarter",0x00BC},{"onesuperior",0x00B9},
    {"ordfeminine",0x00AA},{"ordmasculine",0x00BA},{"oslash",0x00F8},
    {"otilde",0x00F5},{"p",0x0070},{"paragraph",0x00B6},
    {"parenleft",0x0028},{"parenright",0x0029},{"percent",0x0025},
    {"period",0x002E},{"periodcentered",0x00B7},{"perthousand",0x2030},
    {"plus",0x002B},{"plusminus",0x00B1},{"q",0x0071},
    {"question",0x003F},{"questiondown",0x00BF},{"quotedbl",0x0022},
    {"quotedblbase",0x201E},{"quotedblleft",0x201C},
    {"quotedblright",0x201D},{"quoteleft",0x2018},{"quoteright",0x2019},
    {"quotesinglbase",0x201A},{"quotesingle",0x0027},{"r",0x0072},
    {"registered",0x00AE},{"ring",0x02DA},{"s",0x0073},
    {"scaron",0x0161},{"section",0x00A7},{"semicolon",0x003B},
    {"seven",0x0037},{"six",0x0036},{"slash",0x002F},{"space",0x0020},
    {"sterling",0x00A3},{"t",0x0074},{"thorn",0x00FE},
    {"three",0x0033},{"threequarters",0x00BE},{"threesuperior",0x00B3},
    {"tilde",0x02DC},{"trademark",0x2122},{"two",0x0032},
    {"twosuperior",0x00B2},{"u",0x0075},{"uacute",0x00FA},
    {"ucircumflex",0x00FB},{"udieresis",0x00FC},{"ugrave",0x00F9},
    {"underscore",0x005F},{"v",0x0076},{"w",0x0077},{"x",0x0078},
    {"y",0x0079},{"yacute",0x00FD},{"ydieresis",0x00FF},{"yen",0x00A5},
    {"z",0x007A},{"zcaron",0x017E},{"zero",0x0030},
    {NULL, 0}
};

static uint16_t adobe_glyph_to_unicode(const char *name)
{
    for (int i = 0; ADOBE_GLYPH_TABLE[i].name; i++) {
        if (strcmp(ADOBE_GLYPH_TABLE[i].name, name) == 0)
            return ADOBE_GLYPH_TABLE[i].cp;
    }
    return 0;
}

/* ============================================================================
 * /Encoding Dictionary Parsing
 * ============================================================================ */

typedef enum {
    PDF_ENC_WINANSI = 0,
    PDF_ENC_MACROMAN,
    PDF_ENC_STANDARD,   /* treat as WinAnsi for now (mostly overlaps) */
} PdfEncoding;

/* Build encoding table: base encoding + /Differences overrides.
 * Returns the encoding table to use (or NULL to use default WinAnsi). */
static const uint16_t *resolve_encoding(ShPdf2strucCtx *ctx, PdfDict *fd,
                                         uint16_t *custom_table)
{
    PdfObj *enc_obj = pdf_resolve(ctx, pdf_dict_get(fd, "Encoding"));
    if (!enc_obj) return NULL; /* default WinAnsi */

    const uint16_t *base_table = WINANSI_TO_UNICODE;

    if (enc_obj->type == PDF_OBJ_NAME) {
        if (strcmp(enc_obj->name_val, "MacRomanEncoding") == 0)
            base_table = MACROMAN_TO_UNICODE;
        /* WinAnsiEncoding and StandardEncoding both use WinAnsi table */
        /* No /Differences to apply for plain name encoding */
        if (base_table != WINANSI_TO_UNICODE) {
            memcpy(custom_table, base_table, PDF_ENCODING_ENTRIES * sizeof(uint16_t));
            return custom_table;
        }
        return NULL; /* use default */
    }

    if (enc_obj->type != PDF_OBJ_DICT) return NULL;
    PdfDict *enc_d = &enc_obj->dict_val;

    /* Base encoding */
    const char *base_name = pdf_dict_get_name(ctx, enc_d, "BaseEncoding");
    if (base_name && strcmp(base_name, "MacRomanEncoding") == 0)
        base_table = MACROMAN_TO_UNICODE;

    /* Start with base */
    memcpy(custom_table, base_table, PDF_ENCODING_ENTRIES * sizeof(uint16_t));

    /* Apply /Differences array: [code /name /name code /name ...] */
    PdfObj *diff = pdf_resolve(ctx, pdf_dict_get(enc_d, "Differences"));
    if (diff && diff->type == PDF_OBJ_ARRAY) {
        int code = 0;
        int n = pdf_array_len(diff);
        for (int i = 0; i < n; i++) {
            PdfObj *item = pdf_resolve(ctx, pdf_array_get(diff, i));
            if (!item) continue;
            if (item->type == PDF_OBJ_INT) {
                code = (int)item->int_val;
            } else if (item->type == PDF_OBJ_NAME) {
                if (code >= 0 && code < PDF_ENCODING_ENTRIES) {
                    uint16_t cp = adobe_glyph_to_unicode(item->name_val);
                    if (cp > 0) custom_table[code] = cp;
                }
                code++;
            }
        }
    }

    return custom_table;
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

        /* Parse /Encoding (name or dict with /Differences) */
        font->encoding = resolve_encoding(ctx, fd, font->encoding_buf);

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

        if (font && font->has_tounicode) {
            const PdfToUnicodeEntry *e = tounicode_find(font, code);
            if (e && e->text[0]) {
                size_t tlen = strlen(e->text);
                if (out + tlen < len * 4) {
                    memcpy(buf + out, e->text, tlen);
                    out += tlen;
                }
            } else {
                uint32_t cp = e ? e->codepoint : 0xFFFD;
                if (cp > 0 && cp != 0xFFFD)
                    out += (size_t)utf8_encode(cp, buf + out);
            }
        } else {
            const uint16_t *enc_table = (font && font->encoding)
                                         ? font->encoding : WINANSI_TO_UNICODE;
            uint32_t cp = enc_table[code];
            if (cp == 0 && code != 0) cp = code;
            if (cp > 0 && cp != 0xFFFD)
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

        uint32_t cp = 0xFFFD;
        if (font->has_tounicode) {
            const PdfToUnicodeEntry *e = tounicode_find(font, gid);
            if (e && e->text[0]) {
                size_t tlen = strlen(e->text);
                if (out + tlen < max_chars * 4) {
                    memcpy(buf + out, e->text, tlen);
                    out += tlen;
                }
                cp = e->codepoint;
                goto advance_cid;
            }
            cp = e ? e->codepoint : 0xFFFD;
        } else {
            cp = gid; /* Identity mapping fallback */
        }

        if (cp > 0 && cp != 0xFFFD) {
            out += (size_t)utf8_encode(cp, buf + out);
        }
advance_cid:
        ;  /* empty statement after label (C11 compat) */
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
            /* BI/ID/EI: inline image — skip past the data */
            else if (OP_IS("BI")) {
                /* Scan forward to find ID marker */
                while (s.pos < s.size - 1) {
                    if (s.data[s.pos] == 'I' && s.data[s.pos + 1] == 'D' &&
                        (s.pos == 0 || pdf_is_ws(s.data[s.pos - 1]))) {
                        s.pos += 2;
                        /* Skip one whitespace byte after ID */
                        if (s.pos < s.size) s.pos++;
                        /* Scan for EI preceded by whitespace */
                        while (s.pos < s.size - 1) {
                            if (s.data[s.pos] == 'E' && s.data[s.pos + 1] == 'I' &&
                                (s.pos > 0 && pdf_is_ws(s.data[s.pos - 1])) &&
                                (s.pos + 2 >= s.size || pdf_is_ws(s.data[s.pos + 2]))) {
                                s.pos += 2;
                                goto bi_done;
                            }
                            s.pos++;
                        }
                        break;
                    }
                    s.pos++;
                }
                bi_done:
                ops.top = 0;
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

        /* Apply /Rotate to page's initial CTM.
         * Note: process_content_stream starts with identity CTM.
         * For rotated pages the viewer adjusts, but the content stream
         * coordinates are in the original (unrotated) space.
         * We don't adjust CTM here — instead we swapped width/height
         * during page collection so that origin_top_left conversion
         * uses the correct effective dimensions. */

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

    /* Page widths for block output */
    double *page_widths = (double *)sh_arena_alloc(ctx->arena,
                               (size_t)ctx->page_count * sizeof(double));
    if (!page_widths) return SH_PDF2STRUC_ERR_OOM;
    for (int i = 0; i < ctx->page_count; i++) {
        page_widths[i] = ctx->pages[i].width;
    }

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
            blk.page_width = (r->page_index < ctx->page_count) ?
                              page_widths[r->page_index] : 612.0;
            blk.page_height = (r->page_index < ctx->page_count) ?
                               page_heights[r->page_index] : 792.0;
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
            blk.page_width = (first->page_index < ctx->page_count) ?
                              page_widths[first->page_index] : 612.0;
            blk.page_height = (first->page_index < ctx->page_count) ?
                               page_heights[first->page_index] : 792.0;
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
