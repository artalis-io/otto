/*
 * test_pdf2struc.c - Tests for sh_pdf2struc PDF text extraction
 *
 * Tests cover:
 * - API basics (create/destroy, null inputs, options)
 * - PDF header validation
 * - Minimal synthetic PDF parsing
 * - Text operators (Tj, TJ, Td, hex strings, q/Q)
 * - Block merging logic
 * - Multi-page, empty page, encrypted rejection
 *
 * All synthetic PDFs are generated with correct xref offsets/stream lengths.
 */

#include "sh_pdf2struc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))
#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (!_a || !_b || strcmp(_a, _b) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n    at %s:%d\n", \
               _b ? _b : "(null)", _a ? _a : "(null)", __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * Callback Helpers
 * ============================================================================ */

#define MAX_BLOCKS 1024

typedef struct {
    ShPdf2strucBlock blocks[MAX_BLOCKS];
    char text_copies[MAX_BLOCKS][512];
    int count;
} BlockCollector;

static void collect_blocks(void *user, const ShPdf2strucBlock *block)
{
    BlockCollector *bc = (BlockCollector *)user;
    if (bc->count >= MAX_BLOCKS) return;
    bc->blocks[bc->count] = *block;
    snprintf(bc->text_copies[bc->count], 512, "%s", block->text);
    bc->blocks[bc->count].text = bc->text_copies[bc->count];
    bc->count++;
}

/* ============================================================================
 * Synthetic PDFs (generated with correct offsets and stream lengths)
 * ============================================================================ */

/* Single page, one "Hello" text run */
static const char MINIMAL_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 44 >>\nstream\nBT /F1 "
    "12 Tf 1 0 0 1 72 720 Tm (Hello) Tj ET\nendstream\nendobj\n5 0 obj\n<< /Typ"
    "e /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\nxref\n0 6\n000000"
    "0000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000"
    " n \n0000000241 00000 n \n0000000335 00000 n \ntrailer\n<< /Size 6 /Root 1"
    " 0 R >>\nstartxref\n405\n%%EOF\n";

/* Two words: "Hello" at x=72, "World" at x=102 */
static const char TWO_WORD_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 74 >>\nstream\nBT /F1 "
    "12 Tf 1 0 0 1 72 720 Tm (Hello) Tj 1 0 0 1 102 720 Tm (World) Tj ET\nen"
    "dstream\nendobj\n5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helve"
    "tica >>\nendobj\nxref\n0 6\n0000000000 65535 f \n0000000009 00000 n \n000000"
    "0058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n0000000365 00000"
    " n \ntrailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n435\n%%EOF\n";

/* TJ array with kerning: [(AB) -100 (CD)] */
static const char TJ_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 53 >>\nstream\nBT /F1 "
    "12 Tf 1 0 0 1 72 720 Tm [(AB) -100 (CD)] TJ ET\nendstream\nendobj\n5 0 ob"
    "j\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\nxref\n0"
    " 6\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000"
    "115 00000 n \n0000000241 00000 n \n0000000344 00000 n \ntrailer\n<< /Size "
    "6 /Root 1 0 R >>\nstartxref\n414\n%%EOF\n";

/* Td relative positioning: "Row1" then 0 -14 Td "Row2" */
static const char TD_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 62 >>\nstream\nBT /F1 "
    "12 Tf 1 0 0 1 72 720 Tm (Row1) Tj 0 -14 Td (Row2) Tj ET\nendstream\nendo"
    "bj\n5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendo"
    "bj\nxref\n0 6\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n"
    " \n0000000115 00000 n \n0000000241 00000 n \n0000000353 00000 n \ntrailer\n"
    "<< /Size 6 /Root 1 0 R >>\nstartxref\n423\n%%EOF\n";

/* Hex string: <48656C6C6F> = "Hello" */
static const char HEXSTRING_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 49 >>\nstream\nBT /F1 "
    "12 Tf 1 0 0 1 72 720 Tm <48656C6C6F> Tj ET\nendstream\nendobj\n5 0 obj\n<<"
    " /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\nxref\n0 6\n0"
    "000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 "
    "00000 n \n0000000241 00000 n \n0000000340 00000 n \ntrailer\n<< /Size 6 /R"
    "oot 1 0 R >>\nstartxref\n410\n%%EOF\n";

/* Graphics state save/restore: q ... Q */
static const char QQ_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Fon"
    "t << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 81 >>\nstream\nBT /F1 "
    "12 Tf q 1 0 0 1 100 700 Tm (Inside) Tj Q 1 0 0 1 72 720 Tm (Outside) T"
    "j ET\nendstream\nendobj\n5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont"
    " /Helvetica >>\nendobj\nxref\n0 6\n0000000000 65535 f \n0000000009 00000 n "
    "\n0000000058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n000000037"
    "2 00000 n \ntrailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n442\n%%EOF\n";

/* Encrypted PDF: should be rejected */
static const char ENCRYPTED_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [] /Count 0 >>\nendobj\nxref\n0 3\n0000000000 65535 f \n0"
    "000000009 00000 n \n0000000058 00000 n \ntrailer\n<< /Size 3 /Root 1 0 R "
    "/Encrypt << /V 1 >> >>\nstartxref\n110\n%%EOF\n";

/* Two pages with "Page1" and "Page2" */
static const char MULTIPAGE_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>\nendobj\n3 0 obj\n<< /Type /P"
    "age /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources <"
    "< /Font << /F1 5 0 R >> >> >>\nendobj\n4 0 obj\n<< /Length 44 >>\nstream\nB"
    "T /F1 12 Tf 1 0 0 1 72 720 Tm (Page1) Tj ET\nendstream\nendobj\n5 0 obj\n<"
    "< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n6 0 obj\n<"
    "< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 7 0 R /R"
    "esources << /Font << /F1 5 0 R >> >> >>\nendobj\n7 0 obj\n<< /Length 44 >"
    ">\nstream\nBT /F1 12 Tf 1 0 0 1 72 720 Tm (Page2) Tj ET\nendstream\nendobj"
    "\nxref\n0 8\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n"
    "0000000121 00000 n \n0000000247 00000 n \n0000000341 00000 n \n0000000411"
    " 00000 n \n0000000537 00000 n \ntrailer\n<< /Size 8 /Root 1 0 R >>\nstartx"
    "ref\n631\n%%EOF\n";

/* Empty page, no text */
static const char EMPTY_PAGE_PDF[] =
    "%PDF-1.4\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 obj\n<< /"
    "Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /Type /Page /P"
    "arent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << >> >"
    ">\nendobj\n4 0 obj\n<< /Length 0 >>\nstream\n\nendstream\nendobj\nxref\n0 5\n000"
    "0000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00"
    "000 n \n0000000219 00000 n \ntrailer\n<< /Size 5 /Root 1 0 R >>\nstartxref"
    "\n268\n%%EOF\n";

/* ============================================================================
 * API Tests
 * ============================================================================ */

TEST(create_destroy)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ASSERT(ctx != NULL);
    sh_pdf2struc_destroy(ctx);
    sh_pdf2struc_destroy(NULL); /* safe */
}

TEST(opts_default)
{
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    ASSERT_EQ(opt.emit_mode, SH_PDF2STRUC_EMIT_BLOCKS);
    ASSERT_EQ(opt.origin_top_left, 1);
    ASSERT_EQ(opt.approx_widths, 1);
    ASSERT_NEAR(opt.merge_y_epsilon, 1.0, 0.01);
    ASSERT_NEAR(opt.merge_x_gap, 3.0, 0.01);
}

TEST(null_inputs)
{
    ShPdf2strucStatus st;
    st = sh_pdf2struc_extract_mem(NULL, (const uint8_t *)"x", 1, NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_INVALID_PDF);

    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    st = sh_pdf2struc_extract_mem(ctx, NULL, 0, NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_INVALID_PDF);
    sh_pdf2struc_destroy(ctx);

    ctx = sh_pdf2struc_create();
    st = sh_pdf2struc_extract_file(ctx, NULL, NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_IO);
    sh_pdf2struc_destroy(ctx);
}

TEST(invalid_pdf_header)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    const char *data = "This is not a PDF";
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)data, strlen(data), NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_INVALID_PDF);
    ASSERT(strstr(sh_pdf2struc_last_error(ctx), "not a PDF") != NULL);
    sh_pdf2struc_destroy(ctx);
}

TEST(empty_data)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)"", 0, NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_INVALID_PDF);
    sh_pdf2struc_destroy(ctx);
}

TEST(last_error_null_ctx)
{
    const char *err = sh_pdf2struc_last_error(NULL);
    ASSERT(err != NULL);
    ASSERT(strstr(err, "null") != NULL);
}

TEST(file_not_found)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucStatus st = sh_pdf2struc_extract_file(ctx,
        "/nonexistent/path/to/file.pdf", NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_IO);
    sh_pdf2struc_destroy(ctx);
}

TEST(opts_null_safe)
{
    sh_pdf2struc_opts_default(NULL);
}

TEST(status_codes)
{
    ASSERT_EQ(SH_PDF2STRUC_OK, 0);
    ASSERT(SH_PDF2STRUC_ERR_INVALID_PDF != 0);
    ASSERT(SH_PDF2STRUC_ERR_UNSUPPORTED != 0);
    ASSERT(SH_PDF2STRUC_ERR_IO != 0);
    ASSERT(SH_PDF2STRUC_ERR_OOM != 0);
}

TEST(no_callback)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)MINIMAL_PDF, strlen(MINIMAL_PDF),
        &opt, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    sh_pdf2struc_destroy(ctx);
}

/* ============================================================================
 * Minimal PDF Tests
 * ============================================================================ */

TEST(minimal_pdf_parse)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)MINIMAL_PDF, strlen(MINIMAL_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count > 0);
    ASSERT_STREQ(bc.blocks[0].text, "Hello");
    ASSERT_EQ(bc.blocks[0].page_index, 0);
    sh_pdf2struc_destroy(ctx);
}

TEST(minimal_pdf_coordinates)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 0;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)MINIMAL_PDF, strlen(MINIMAL_PDF),
        &opt, collect_blocks, &bc);

    ASSERT(bc.count > 0);
    ASSERT_NEAR(bc.blocks[0].x, 72.0, 1.0);
    ASSERT(bc.blocks[0].h > 0);
    ASSERT(bc.blocks[0].w > 0);
    sh_pdf2struc_destroy(ctx);
}

TEST(minimal_pdf_top_left_origin)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 1;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)MINIMAL_PDF, strlen(MINIMAL_PDF),
        &opt, collect_blocks, &bc);

    ASSERT(bc.count > 0);
    /* With top-left origin, y = page_height - bottom_y */
    /* Page height 792, text bottom at ~717.6, height ~14.4 → top-left y ~60 */
    ASSERT(bc.blocks[0].y > 50.0 && bc.blocks[0].y < 100.0);
    sh_pdf2struc_destroy(ctx);
}

/* ============================================================================
 * Text Operator Tests
 * ============================================================================ */

TEST(two_word_merge)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opt.merge_x_gap = 50.0;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)TWO_WORD_PDF, strlen(TWO_WORD_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 1);
    ASSERT(strstr(bc.blocks[0].text, "Hello") != NULL);
    ASSERT(strstr(bc.blocks[0].text, "World") != NULL);
    sh_pdf2struc_destroy(ctx);
}

TEST(two_word_separate)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)TWO_WORD_PDF, strlen(TWO_WORD_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 2);
    ASSERT_STREQ(bc.blocks[0].text, "Hello");
    ASSERT_STREQ(bc.blocks[1].text, "World");
    sh_pdf2struc_destroy(ctx);
}

TEST(tj_kerning)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opt.merge_x_gap = 50.0;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)TJ_PDF, strlen(TJ_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 1);
    ASSERT(strstr(bc.blocks[0].text, "AB") != NULL);
    ASSERT(strstr(bc.blocks[0].text, "CD") != NULL);
    sh_pdf2struc_destroy(ctx);
}

TEST(td_relative_position)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 0;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)TD_PDF, strlen(TD_PDF),
        &opt, collect_blocks, &bc);

    ASSERT(bc.count >= 2);
    int found_row1 = 0, found_row2 = 0;
    double y1 = 0, y2 = 0;
    for (int i = 0; i < bc.count; i++) {
        if (strcmp(bc.blocks[i].text, "Row1") == 0) { found_row1 = 1; y1 = bc.blocks[i].y; }
        if (strcmp(bc.blocks[i].text, "Row2") == 0) { found_row2 = 1; y2 = bc.blocks[i].y; }
    }
    ASSERT(found_row1);
    ASSERT(found_row2);
    /* Row2 should be ~14 points below Row1 in PDF coords */
    ASSERT_NEAR(y1 - y2, 14.0, 2.0);
    sh_pdf2struc_destroy(ctx);
}

TEST(hex_string_decode)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)HEXSTRING_PDF, strlen(HEXSTRING_PDF),
        &opt, collect_blocks, &bc);

    ASSERT(bc.count >= 1);
    ASSERT_STREQ(bc.blocks[0].text, "Hello");
    sh_pdf2struc_destroy(ctx);
}

TEST(graphics_state_save_restore)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)QQ_PDF, strlen(QQ_PDF),
        &opt, collect_blocks, &bc);

    int found_inside = 0, found_outside = 0;
    for (int i = 0; i < bc.count; i++) {
        if (strcmp(bc.blocks[i].text, "Inside") == 0) found_inside = 1;
        if (strcmp(bc.blocks[i].text, "Outside") == 0) found_outside = 1;
    }
    ASSERT(found_inside);
    ASSERT(found_outside);
    sh_pdf2struc_destroy(ctx);
}

/* ============================================================================
 * Multi-page / Edge Case Tests
 * ============================================================================ */

TEST(multipage)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)MULTIPAGE_PDF, strlen(MULTIPAGE_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 2);

    int found_p0 = 0, found_p1 = 0;
    for (int i = 0; i < bc.count; i++) {
        if (bc.blocks[i].page_index == 0 && strcmp(bc.blocks[i].text, "Page1") == 0)
            found_p0 = 1;
        if (bc.blocks[i].page_index == 1 && strcmp(bc.blocks[i].text, "Page2") == 0)
            found_p1 = 1;
    }
    ASSERT(found_p0);
    ASSERT(found_p1);
    sh_pdf2struc_destroy(ctx);
}

TEST(empty_page)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)EMPTY_PAGE_PDF, strlen(EMPTY_PAGE_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT_EQ(bc.count, 0);
    sh_pdf2struc_destroy(ctx);
}

TEST(encrypted_rejected)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)ENCRYPTED_PDF, strlen(ENCRYPTED_PDF),
        NULL, NULL, NULL);
    ASSERT_EQ(st, SH_PDF2STRUC_ERR_UNSUPPORTED);
    sh_pdf2struc_destroy(ctx);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nsh_pdf2struc tests\n");
    printf("==================\n\n");

    printf("API tests:\n");
    RUN_TEST(create_destroy);
    RUN_TEST(opts_default);
    RUN_TEST(null_inputs);
    RUN_TEST(invalid_pdf_header);
    RUN_TEST(empty_data);
    RUN_TEST(last_error_null_ctx);
    RUN_TEST(file_not_found);
    RUN_TEST(opts_null_safe);
    RUN_TEST(status_codes);
    RUN_TEST(no_callback);

    printf("\nMinimal PDF tests:\n");
    RUN_TEST(minimal_pdf_parse);
    RUN_TEST(minimal_pdf_coordinates);
    RUN_TEST(minimal_pdf_top_left_origin);

    printf("\nText operator tests:\n");
    RUN_TEST(two_word_merge);
    RUN_TEST(two_word_separate);
    RUN_TEST(tj_kerning);
    RUN_TEST(td_relative_position);
    RUN_TEST(hex_string_decode);
    RUN_TEST(graphics_state_save_restore);

    printf("\nMulti-page tests:\n");
    RUN_TEST(multipage);
    RUN_TEST(empty_page);

    printf("\nEncryption tests:\n");
    RUN_TEST(encrypted_rejected);

    printf("\n==================\n");
    printf("%d/%d tests passed\n\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
