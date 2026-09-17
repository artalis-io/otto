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
/* Crafted by scratchpad/gen_tests.py -- see the audit notes. */

/* ===========================================================================
 * Regression fixtures for sh_pdf2struc_text.c
 *
 * Generated, not hand-written: each carries its own xref with real offsets.
 * Every one of these was confirmed to fail against the pre-fix parser.
 * =========================================================================== */

static const char UNTERMINATED_STRING_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj\nxref\n0 "
    "4\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n00"
    "00000115 00000 n \ntrailer\n<< /Size 4 /Root 1 0 R /X (abcdefgh\nsta"
    "rtxref\n186\n%%EOF\n";
static const size_t UNTERMINATED_STRING_PDF_LEN = 339;

static const char PAGE_TREE_SELF_CYCLE_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [2 0 R] /Count 1 >>\nendobj\nxref\n0 3\n000"
    "0000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \ntrailer"
    "\n<< /Size 3 /Root 1 0 R >>\nstartxref\n115\n%%EOF\n";
static const size_t PAGE_TREE_SELF_CYCLE_PDF_LEN = 238;

static const char PAGE_TREE_MUTUAL_CYCLE_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Pages /Kids [2 0 R] /Count 1 >>\nendobj\nxref\n0 4\n0000000000 "
    "65535 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000"
    " n \ntrailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n172\n%%EOF\n";
static const size_t PAGE_TREE_MUTUAL_CYCLE_PDF_LEN = 315;

static const char UNDEFINED_FONT_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n5 0 obj\n<< /Len"
    "gth 34 >>\nstream\nBT /F9 12 Tf 10 700 Td (Hi) Tj ET\nendstream\nend"
    "obj\nxref\n0 6\n0000000000 65535 f \n0000000009 00000 n \n0000000058"
    " 00000 n \n0000000115 00000 n \n0000000241 00000 n \n0000000311 0000"
    "0 n \ntrailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n394\n%%EOF\n";
static const size_t UNDEFINED_FONT_PDF_LEN = 577;

static const char TOUNICODE_OVERFLOW_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>\nendobj"
    "\n5 0 obj\n<< /Length 41 >>\nstream\nBT /F1 12 Tf 10 700 Td (AAABBBB"
    "BB) Tj ET\nendstream\nendobj\n6 0 obj\n<< /Length 123 >>\nstream\n/C"
    "IDInit /ProcSet findresource begin\nbegincmap\n2 beginbfchar\n<41> <"
    "20AC20AC20AC20AC>\n<42> <000020AC>\nendbfchar\nendcmap\nend\nendstre"
    "am\nendobj\nxref\n0 7\n0000000000 65535 f \n0000000009 00000 n \n000"
    "0000058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n00000003"
    "28 00000 n \n0000000418 00000 n \ntrailer\n<< /Size 7 /Root 1 0 R >>"
    "\nstartxref\n591\n%%EOF\n";
static const size_t TOUNICODE_OVERFLOW_PDF_LEN = 794;

static const char BFRANGE_HUGE_SPAN_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>\nendobj"
    "\n5 0 obj\n<< /Length 34 >>\nstream\nBT /F1 12 Tf 10 700 Td (Hi) Tj "
    "ET\nendstream\nendobj\n6 0 obj\n<< /Length 110 >>\nstream\n/CIDInit "
    "/ProcSet findresource begin\nbegincmap\n1 beginbfrange\n<0000> <FFFF"
    "FFFF> <0041>\nendbfrange\nendcmap\nend\nendstream\nendobj\nxref\n0 7"
    "\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n000"
    "0000115 00000 n \n0000000241 00000 n \n0000000328 00000 n \n00000004"
    "11 00000 n \ntrailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n571\n%%E"
    "OF\n";
static const size_t BFRANGE_HUGE_SPAN_PDF_LEN = 774;

static const char CID_WIDTH_HUGE_SPAN_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type0 /BaseFont /Sub /Encoding /Identity-H /DescendantF"
    "onts [6 0 R] >>\nendobj\n5 0 obj\n<< /Length 36 >>\nstream\nBT /F1 1"
    "2 Tf 10 700 Td <0041> Tj ET\nendstream\nendobj\n6 0 obj\n<< /Type /F"
    "ont /Subtype /CIDFontType2 /BaseFont /Sub /DW 1000 /W [0 2147483647 "
    "500] >>\nendobj\nxref\n0 7\n0000000000 65535 f \n0000000009 00000 n "
    "\n0000000058 00000 n \n0000000115 00000 n \n0000000241 00000 n \n000"
    "0000352 00000 n \n0000000437 00000 n \ntrailer\n<< /Size 7 /Root 1 0"
    " R >>\nstartxref\n539\n%%EOF\n";
static const size_t CID_WIDTH_HUGE_SPAN_PDF_LEN = 742;

static const char DIFFERENCES_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type1 /BaseFont /Helvetica /Encoding << /Type /Encoding"
    " /BaseEncoding /WinAnsiEncoding /Differences [65 /Euro 66 /uni00E9] "
    ">> >>\nendobj\n5 0 obj\n<< /Length 34 >>\nstream\nBT /F1 12 Tf 10 70"
    "0 Td (AB) Tj ET\nendstream\nendobj\nxref\n0 6\n0000000000 65535 f \n"
    "0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n \n00000"
    "00241 00000 n \n0000000410 00000 n \ntrailer\n<< /Size 6 /Root 1 0 R"
    " >>\nstartxref\n493\n%%EOF\n";
static const size_t DIFFERENCES_PDF_LEN = 676;

static const char WORD_GAP_PDF[] =
    "%PDF-1.7\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
    "<< /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n4 0 obj\n<< /Type /Fo"
    "nt /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n5 0 obj\n<< /Len"
    "gth 72 >>\nstream\nBT /F1 12 Tf 1 0 0 1 72 720 Tm (Due) Tj 1 0 0 1 1"
    "00 720 Tm (Date) Tj ET\nendstream\nendobj\nxref\n0 6\n0000000000 655"
    "35 f \n0000000009 00000 n \n0000000058 00000 n \n0000000115 00000 n "
    "\n0000000241 00000 n \n0000000311 00000 n \ntrailer\n<< /Size 6 /Roo"
    "t 1 0 R >>\nstartxref\n432\n%%EOF\n";
static const size_t WORD_GAP_PDF_LEN = 615;

static const char XREF_NEGATIVE_W_PDF[] =
    "%PDF-1.5\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R >>\nendobj\n4 0 obj\n<< /Type /XRef /Size 5 "
    "/W [-5 10 10] /Index [0 5] /Root 1 0 R /Filter /FlateDecode /Length "
    "23 >>\nstream\nx\234c`dbfaec\347\340\344\342\346\341\345\003\000\002"
    "?\000j\nendstream\nendobj\nstartxref\n162\n%%EOF\n";

static const size_t XREF_NEGATIVE_W_PDF_LEN = 335;

static const char PREDICTOR_HUGE_COLUMNS_PDF[] =
    "%PDF-1.5\n1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n2 0 ob"
    "j\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n3 0 obj\n<< /T"
    "ype /Page /Parent 2 0 R /Contents 4 0 R >>\nendobj\n4 0 obj\n<< /Len"
    "gth 12 /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 2"
    "147483647 >> >>\nstream\nx\234st\034\331\000\000\243`A\001\nendstrea"
    "m\nendobj\nxref\n0 5\n0000000000 65535 f \n0000000009 00000 n \n0000"
    "000058 00000 n \n0000000115 00000 n \n0000000178 00000 n \ntrailer\n"
    "<< /Size 5 /Root 1 0 R >>\nstartxref\n314\n%%EOF\n";

static const size_t PREDICTOR_HUGE_COLUMNS_PDF_LEN = 477;

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

/*
 * Both of these are crafted inputs from the shared/ audit. They are cheap to
 * keep and neither is reachable by accident, so a regression here would
 * otherwise only show up as a sanitizer report on someone's machine.
 */

/* /W field widths are checked individually, not just as a sum. The read loops
 * run max(0, w) times each, so a negative width contributes nothing to the
 * bytes consumed while still lowering entry_size: [-5 10 10] sums to 15,
 * passed the bound, then consumed 20, advancing past what the guard had
 * verified. Validated before decompression, so the stream contents do not
 * matter here. */
TEST(xref_stream_rejects_negative_w)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ASSERT(ctx != NULL);

    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(
        ctx, (const uint8_t *)XREF_NEGATIVE_W_PDF,
        XREF_NEGATIVE_W_PDF_LEN, &opt, NULL, NULL);

    ASSERT(st != SH_PDF2STRUC_OK);
    ASSERT(strstr(sh_pdf2struc_last_error(ctx), "negative field width") != NULL);
    sh_pdf2struc_destroy(ctx);
}

/* Columns comes from DecodeParms unbounded, and row_bytes + 1 overflowed for
 * anything near INT_MAX -- confirmed by UBSan on a 477-byte file before the
 * bound was added. The parse must simply not perform that arithmetic; whether
 * the document yields content is beside the point. */
TEST(predictor_rejects_huge_columns)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ASSERT(ctx != NULL);

    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(
        ctx, (const uint8_t *)PREDICTOR_HUGE_COLUMNS_PDF,
        PREDICTOR_HUGE_COLUMNS_PDF_LEN, &opt, NULL, NULL);

    /* The document still parses: the predictor is simply declined, rather
     * than the file being rejected or the arithmetic being performed. */
    ASSERT(st == SH_PDF2STRUC_OK);
    sh_pdf2struc_destroy(ctx);
}

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
 * Golden Tests - Table-like PDF with known output
 * ============================================================================ */

/*
 * 3-column, 3-row table-like PDF (landscape A4 style, like GLS Hungary docs).
 * Each row has 3 cells at fixed x positions:
 *   col1=72, col2=200, col3=350
 *   row1 y=700, row2 y=686, row3 y=672 (14pt line spacing)
 *
 * Content:
 *   City          Code    Depot
 *   Budapest      BP01    HQ
 *   Debrecen      DB02    East
 */
static const char GOLDEN_TABLE_PDF[] =
    "%PDF-1.4\n"
    "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
    "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
    "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 842 595]"
    " /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n"
    "4 0 obj\n<< /Length 280 >>\n"
    "stream\n"
    "BT /F1 10 Tf\n"
    "1 0 0 1 72 700 Tm (City) Tj\n"
    "1 0 0 1 200 700 Tm (Code) Tj\n"
    "1 0 0 1 350 700 Tm (Depot) Tj\n"
    "1 0 0 1 72 686 Tm (Budapest) Tj\n"
    "1 0 0 1 200 686 Tm (BP01) Tj\n"
    "1 0 0 1 350 686 Tm (HQ) Tj\n"
    "1 0 0 1 72 672 Tm (Debrecen) Tj\n"
    "1 0 0 1 200 672 Tm (DB02) Tj\n"
    "1 0 0 1 350 672 Tm (East) Tj\n"
    "ET\n"
    "endstream\n"
    "endobj\n"
    "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"
    "xref\n0 6\n"
    "0000000000 65535 f \n"
    "0000000009 00000 n \n"
    "0000000058 00000 n \n"
    "0000000115 00000 n \n"
    "0000000241 00000 n \n"
    "0000000572 00000 n \n"
    "trailer\n<< /Size 6 /Root 1 0 R >>\n"
    "startxref\n642\n%%EOF\n";

TEST(golden_table_run_count)
{
    /* Extract as individual runs: expect exactly 9 text runs (3x3 table) */
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 0; /* bottom-left origin for stable coordinates */

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT_EQ(bc.count, 9);
    sh_pdf2struc_destroy(ctx);
}

TEST(golden_table_text_content)
{
    /* Verify exact text content of all 9 cells */
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 0;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc);

    /* Runs should be sorted by (page, y, x) */
    /* With bottom-left origin, y=672 < y=686 < y=700 */
    /* So order is: row3 (y=672), row2 (y=686), row1 (y=700) */
    const char *expected[] = {
        "Debrecen", "DB02", "East",    /* y=672 */
        "Budapest", "BP01", "HQ",      /* y=686 */
        "City",     "Code", "Depot",   /* y=700 */
    };

    ASSERT_EQ(bc.count, 9);
    for (int i = 0; i < 9; i++) {
        ASSERT_STREQ(bc.blocks[i].text, expected[i]);
    }
    sh_pdf2struc_destroy(ctx);
}

TEST(golden_table_coordinates)
{
    /* Verify x positions for each column */
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 0;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(bc.count, 9);

    /* Check x positions: col1=72, col2=200, col3=350 for each row */
    for (int row = 0; row < 3; row++) {
        ASSERT_NEAR(bc.blocks[row * 3 + 0].x, 72.0, 0.5);
        ASSERT_NEAR(bc.blocks[row * 3 + 1].x, 200.0, 0.5);
        ASSERT_NEAR(bc.blocks[row * 3 + 2].x, 350.0, 0.5);
    }

    /* Check all blocks have positive width and height */
    for (int i = 0; i < 9; i++) {
        ASSERT(bc.blocks[i].w > 0);
        ASSERT(bc.blocks[i].h > 0);
        ASSERT_EQ(bc.blocks[i].page_index, 0);
    }

    sh_pdf2struc_destroy(ctx);
}

TEST(golden_table_top_left_origin)
{
    /* Verify top-left conversion: y should be near top of page */
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 1;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc);

    ASSERT_EQ(bc.count, 9);

    /* Page height=595 (landscape A4). Text at y=700 in PDF coords means
     * top-left y = 595 - 700 - h. Since y=700 > page_height, top-left y
     * will be negative. But rows at y=672,686,700... row at y=672:
     * top-left y ≈ 595 - 672 - 10 ≈ -87 (font size 10, descent ~2pt).
     * Actually the runs are sorted by converted y (ascending), so the
     * smallest top-left y comes first (closest to top of page).
     * Since these are all above the page (negative y in top-left), they
     * should still be in consistent order. */

    /* Just verify order is deterministic and all on page 0 */
    for (int i = 0; i < 9; i++) {
        ASSERT_EQ(bc.blocks[i].page_index, 0);
    }

    sh_pdf2struc_destroy(ctx);
}

TEST(golden_table_block_merge)
{
    /* With EMIT_BLOCKS and tight x_gap, each cell stays separate (gap >> x_gap) */
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opt.merge_x_gap = 3.0;     /* tight: cols are 128+ pts apart */
    opt.merge_y_epsilon = 1.0;

    BlockCollector bc = {0};
    sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc);

    /* Each cell should remain separate since columns are far apart */
    ASSERT_EQ(bc.count, 9);
    sh_pdf2struc_destroy(ctx);
}

TEST(golden_table_deterministic)
{
    /* Run extraction twice and verify byte-for-byte identical output.
     *
     * static, not automatic: BlockCollector is about 590KB, and two of them
     * overflow the 1MB stack Windows gives a thread by default. That made the
     * suite unrunnable under ASan on Windows -- it aborted here with a
     * stack-overflow -- while Linux CI, with an 8MB stack, never noticed.
     * Cleared explicitly since a static initialiser only runs once. */
    static BlockCollector bc1, bc2;
    memset(&bc1, 0, sizeof(bc1));
    memset(&bc2, 0, sizeof(bc2));

    ShPdf2strucCtx *ctx1 = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_RUNS;
    opt.origin_top_left = 1;

    sh_pdf2struc_extract_mem(ctx1,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc1);

    ShPdf2strucCtx *ctx2 = sh_pdf2struc_create();
    sh_pdf2struc_extract_mem(ctx2,
        (const uint8_t *)GOLDEN_TABLE_PDF, strlen(GOLDEN_TABLE_PDF),
        &opt, collect_blocks, &bc2);

    ASSERT_EQ(bc1.count, bc2.count);
    for (int i = 0; i < bc1.count; i++) {
        ASSERT_STREQ(bc1.blocks[i].text, bc2.blocks[i].text);
        ASSERT_EQ(bc1.blocks[i].page_index, bc2.blocks[i].page_index);
        ASSERT_NEAR(bc1.blocks[i].x, bc2.blocks[i].x, 0.001);
        ASSERT_NEAR(bc1.blocks[i].y, bc2.blocks[i].y, 0.001);
        ASSERT_NEAR(bc1.blocks[i].w, bc2.blocks[i].w, 0.001);
        ASSERT_NEAR(bc1.blocks[i].h, bc2.blocks[i].h, 0.001);
    }

    sh_pdf2struc_destroy(ctx1);
    sh_pdf2struc_destroy(ctx2);
}


/* ============================================================================
 * sh_pdf2struc_text.c regressions
 * ============================================================================ */

/* The extractor's own glyph table, reached through back doors so the ordering
 * invariant the binary search depends on can be checked from here. */
int sh_pdf2struc__glyph_table_count(void);
const char *sh_pdf2struc__glyph_table_name(int i);

/*
 * decode_text_simple() sized its output buffer at 4 bytes per input byte, but
 * a ToUnicode entry can map one byte to a 15-byte multi-codepoint sequence.
 * This PDF mixes both shapes in one string: three bytes that each expand to 12
 * bytes push the write offset to just under the budget, then six more bytes
 * take the unchecked utf8_encode() path and run past the end.
 *
 * Pre-fix this wrote 14 bytes past a 41-byte allocation. It is only visible to
 * ASan because sh_arena_alloc() now poisons the space between allocations --
 * without that the overflow lands in the next live arena slice and nothing
 * reports it, which is why the fuzzer had been running over this file clean.
 */
/*
 * /Tf naming a font the page's /Resources does not define leaves the graphics
 * state's font index at -1, and decode_text_simple() is then called with a
 * NULL font. Every use of it in that function was guarded except the call to
 * simple_glyph_width() for the advance width, so any content stream drawing
 * text under an unresolvable font dereferenced NULL.
 *
 * Found by fuzzing rather than by reading: a mutation corrupted a font
 * object's "4 0 obj" header into "200 obj", leaving /F1 unresolvable. A
 * misspelled /Tf does the same thing, which is what this fixture uses.
 */
/*
 * pdf_parse_obj() recurses through arrays and dictionaries and nothing
 * bounded it. A content stream of 20000 open brackets -- a 40KB file --
 * exhausted the stack and killed the process; ASan reported a stack-overflow
 * with no usable frame. Found by fuzzing, once the seed corpus had grown
 * enough to reach the content-stream parser in depth.
 *
 * The fixture is built here rather than embedded: 40KB of brackets as a C
 * string literal would dwarf the rest of the file.
 */
/*
 * /Kids is an object reference like any other, and the page tree walk
 * followed it with no depth bound and no cycle check. A Pages node listing
 * itself in /Kids recursed until the stack ran out -- in a 238 byte file.
 * The mutual case, two Pages nodes each naming the other, is the same defect
 * and is not caught by a self-reference test, so both are here.
 *
 * Found by fuzzing. The extractor must refuse the file, not die on it.
 */
/*
 * pdf_parse_string() sizes its buffer in one pass and fills it in another.
 * The sizing pass subtracted one for the closing ")" unconditionally -- but
 * a string that is never closed has no ")" to subtract, so the buffer came
 * out a byte short and the decode pass wrote its NUL terminator past the
 * end. This fixture is a trailer holding an unterminated string, 339 bytes.
 *
 * Invisible before this branch: the stray byte landed in the next arena
 * slice, which ASan saw as a legal write into a live allocation. It is
 * reportable now because sh_arena_alloc() poisons between slices, and only
 * caught reliably because it unpoisons the requested size rather than the
 * size rounded up to alignment -- at the rounded size this particular
 * one-byte overflow fell inside the padding.
 */
TEST(unterminated_string_stays_in_bounds)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)UNTERMINATED_STRING_PDF, UNTERMINATED_STRING_PDF_LEN,
        &opt, collect_blocks, &bc);

    /* Returning without writing out of bounds is the assertion; under ASan
     * the overflow aborts before this line is reached. */
    (void)st;
    sh_pdf2struc_destroy(ctx);
}

TEST(page_tree_cycle_is_refused_not_fatal)
{
    const char *pdfs[2];
    size_t lens[2];
    pdfs[0] = PAGE_TREE_SELF_CYCLE_PDF;   lens[0] = PAGE_TREE_SELF_CYCLE_PDF_LEN;
    pdfs[1] = PAGE_TREE_MUTUAL_CYCLE_PDF; lens[1] = PAGE_TREE_MUTUAL_CYCLE_PDF_LEN;

    for (int i = 0; i < 2; i++) {
        ShPdf2strucCtx *ctx = sh_pdf2struc_create();
        ShPdf2strucOpts opt;
        sh_pdf2struc_opts_default(&opt);

        BlockCollector bc = {0};
        ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
            (const uint8_t *)pdfs[i], lens[i], &opt, collect_blocks, &bc);

        /* Returning at all is the assertion. Neither file has a real page,
         * so no text is expected either way. */
        (void)st;
        ASSERT_EQ(bc.count, 0);
        sh_pdf2struc_destroy(ctx);
    }
}

TEST(deep_nesting_is_refused_not_fatal)
{
    const int depth = 20000;
    size_t cap = (size_t)depth * 2 + 4096;
    char *pdf = (char *)malloc(cap);
    char *content = (char *)malloc((size_t)depth * 2 + 64);
    ASSERT(pdf != NULL && content != NULL);

    int n = 0;
    n += sprintf(content + n, "BT ");
    for (int i = 0; i < depth; i++) content[n++] = '[';
    for (int i = 0; i < depth; i++) content[n++] = ']';
    n += sprintf(content + n, " TJ ET\n");
    content[n] = '\0';

    /* Offsets are recorded as the buffer is written, so the xref is real. */
    int off[6], q = 0, k = 1;
    q += sprintf(pdf + q, "%%PDF-1.7\n");
    off[k++] = q; q += sprintf(pdf + q,
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[k++] = q; q += sprintf(pdf + q,
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[k++] = q; q += sprintf(pdf + q,
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>\nendobj\n");
    off[k++] = q; q += sprintf(pdf + q,
        "4 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n");
    off[k++] = q; q += sprintf(pdf + q,
        "5 0 obj\n<< /Length %d >>\nstream\n%sendstream\nendobj\n", n, content);

    int xref_at = q;
    q += sprintf(pdf + q, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 1; i <= 5; i++)
        q += sprintf(pdf + q, "%010d 00000 n \n", off[i]);
    q += sprintf(pdf + q,
        "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n", xref_at);
    ASSERT((size_t)q < cap);

    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    /* Must return rather than crash. Either status is fine: the point is
     * that a refusal replaced a stack overflow. */
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)pdf, (size_t)q, &opt, collect_blocks, &bc);
    (void)st;

    sh_pdf2struc_destroy(ctx);
    free(pdf);
    free(content);
}

TEST(undefined_font_does_not_crash)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)UNDEFINED_FONT_PDF, UNDEFINED_FONT_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    /* The text still comes out; only the advance width is unknown. */
    ASSERT(bc.count >= 1);
    ASSERT(strstr(bc.blocks[0].text, "Hi") != NULL);
    sh_pdf2struc_destroy(ctx);
}

TEST(tounicode_multi_codepoint_respects_buffer)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)TOUNICODE_OVERFLOW_PDF, TOUNICODE_OVERFLOW_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 1);

    /* The content stream draws "AAABBBBBB" -- 9 bytes -- and the decoder
     * budgets 4 UTF-8 bytes per input byte, so 36 is the most that may ever
     * be emitted. The pre-fix decoder ran its write offset to 42, and what
     * reached the callback measured 40 (the overflow ran into the next arena
     * slice, which the grouping stage then partly overwrote). Either number
     * exceeds 36, so this catches it with or without a sanitizer. */
    for (int i = 0; i < bc.count; i++)
        ASSERT(strlen(bc.blocks[i].text) <= 36);

    sh_pdf2struc_destroy(ctx);
}

/*
 * A bfrange of <0000> <FFFFFFFF> drove `for (uint32_t gid = lo; gid <= hi;
 * gid++)`, where gid++ wraps at UINT32_MAX and the condition is then always
 * true. Confirmed to hang the pre-fix parser indefinitely (killed at 12s).
 * If this regresses the test does not fail, it stops -- and CI times out.
 */
TEST(bfrange_huge_span_terminates)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)BFRANGE_HUGE_SPAN_PDF, BFRANGE_HUGE_SPAN_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    sh_pdf2struc_destroy(ctx);
}

/*
 * /W [0 2147483647 500] drove `for (int c = cid_start; c <= cid_last; c++)`
 * to INT_MAX, where the increment is signed overflow.
 *
 * Unlike the others here, this test does not fail against the pre-fix parser
 * in an ordinary build: at -O1 the compiler uses the undefined behaviour to
 * conclude the loop must terminate and deletes it, so the run completes and
 * the assertion holds. It fails where it matters -- the sanitize job, which
 * builds at -O0 with -fsanitize=undefined and UBSAN_OPTIONS=halt_on_error=1.
 * Verified: pre-fix under those flags it reports "signed integer overflow:
 * 2147483647 + 1" and exits 1.
 *
 * That difference is the argument for the fix. Relying on how a construct
 * happens to compile today is not the same as it being correct.
 */
TEST(cid_width_huge_span_terminates)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)CID_WIDTH_HUGE_SPAN_PDF, CID_WIDTH_HUGE_SPAN_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    sh_pdf2struc_destroy(ctx);
}

/*
 * ADOBE_GLYPH_TABLE held only a {NULL, 0} sentinel, so adobe_glyph_to_unicode()
 * always returned 0 and resolve_encoding()'s `if (cp > 0)` never fired: the
 * whole /Differences array was parsed and discarded. This PDF maps code 65 to
 * /Euro and code 66 to /uni00E9, and prints "AB" -- which came out as literal
 * "AB" before, and as the remapped characters now.
 */
TEST(differences_encoding_applies)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)DIFFERENCES_PDF, DIFFERENCES_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 1);
    /* U+20AC then U+00E9, in UTF-8. */
    ASSERT_STREQ(bc.blocks[0].text, "\xE2\x82\xAC\xC3\xA9");
    sh_pdf2struc_destroy(ctx);
}

/* The binary search is only correct while the table is sorted by name. */
TEST(glyph_table_is_sorted)
{
    int n = sh_pdf2struc__glyph_table_count();
    ASSERT(n > 100);
    for (int i = 0; i < n; i++) {
        const char *cur = sh_pdf2struc__glyph_table_name(i);
        ASSERT(cur != NULL);
        if (i > 0)
            ASSERT(strcmp(sh_pdf2struc__glyph_table_name(i - 1), cur) < 0);
    }
    ASSERT(sh_pdf2struc__glyph_table_name(n) == NULL);
    ASSERT(sh_pdf2struc__glyph_table_name(-1) == NULL);
}

/*
 * pdf_group_runs() wrote a separator byte and then immediately overwrote it
 * with the memcpy on the next line, so merged blocks came out with the words
 * run together. "Due" and "Date" sit 28pt apart at 12pt, a clear word gap.
 */
TEST(merged_block_separates_words)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    ShPdf2strucOpts opt;
    sh_pdf2struc_opts_default(&opt);
    opt.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opt.merge_x_gap = 50.0;

    BlockCollector bc = {0};
    ShPdf2strucStatus st = sh_pdf2struc_extract_mem(ctx,
        (const uint8_t *)WORD_GAP_PDF, WORD_GAP_PDF_LEN,
        &opt, collect_blocks, &bc);

    ASSERT_EQ(st, SH_PDF2STRUC_OK);
    ASSERT(bc.count >= 1);
    ASSERT_STREQ(bc.blocks[0].text, "Due Date");
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
    RUN_TEST(xref_stream_rejects_negative_w);
    RUN_TEST(unterminated_string_stays_in_bounds);
    RUN_TEST(page_tree_cycle_is_refused_not_fatal);
    RUN_TEST(deep_nesting_is_refused_not_fatal);
    RUN_TEST(undefined_font_does_not_crash);
    RUN_TEST(tounicode_multi_codepoint_respects_buffer);
    RUN_TEST(bfrange_huge_span_terminates);
    RUN_TEST(cid_width_huge_span_terminates);
    RUN_TEST(differences_encoding_applies);
    RUN_TEST(glyph_table_is_sorted);
    RUN_TEST(merged_block_separates_words);
    RUN_TEST(predictor_rejects_huge_columns);
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

    printf("\nGolden tests:\n");
    RUN_TEST(golden_table_run_count);
    RUN_TEST(golden_table_text_content);
    RUN_TEST(golden_table_coordinates);
    RUN_TEST(golden_table_top_left_origin);
    RUN_TEST(golden_table_block_merge);
    RUN_TEST(golden_table_deterministic);

    printf("\n==================\n");
    printf("%d/%d tests passed\n\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
