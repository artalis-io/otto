/*
 * test_ingest.c - Tests for Nexus ingestion pipeline
 */

#include "nx_xlsx.h"
#include "nx_pdf.h"
#include "nx_csv.h"
#include "nx_ingest.h"
#include "sh_json.h"
#include "sh_arena.h"
#include "sh_hash_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Embedded minimal XLSX fixture */
#include "fixtures/minimal_xlsx.h"

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
#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n    at %s:%d\n", \
               _b, _a, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * PDF Text-Run Fixture (inline)
 * ============================================================================ */

static const char PDF_FIXTURE[] =
    "{\"pages\":[{\"page\":1,\"width\":595.0,\"height\":842.0,\"texts\":["
    "{\"text\":\"City\",\"x\":50.0,\"y\":100.0,\"w\":40.0,\"h\":12.0},"
    "{\"text\":\"Name\",\"x\":150.0,\"y\":100.0,\"w\":50.0,\"h\":12.0},"
    "{\"text\":\"Lat\",\"x\":300.0,\"y\":100.0,\"w\":30.0,\"h\":12.0},"
    "{\"text\":\"Lon\",\"x\":400.0,\"y\":100.0,\"w\":30.0,\"h\":12.0},"
    "{\"text\":\"Budapest\",\"x\":50.0,\"y\":120.0,\"w\":70.0,\"h\":12.0},"
    "{\"text\":\"Depot #1\",\"x\":150.0,\"y\":120.0,\"w\":60.0,\"h\":12.0},"
    "{\"text\":\"47.4799\",\"x\":300.0,\"y\":120.0,\"w\":50.0,\"h\":12.0},"
    "{\"text\":\"19.0700\",\"x\":400.0,\"y\":120.0,\"w\":50.0,\"h\":12.0},"
    "{\"text\":\"Debrecen\",\"x\":50.0,\"y\":140.0,\"w\":70.0,\"h\":12.0},"
    "{\"text\":\"Depot #2\",\"x\":150.0,\"y\":140.0,\"w\":60.0,\"h\":12.0},"
    "{\"text\":\"47.5316\",\"x\":300.0,\"y\":140.0,\"w\":50.0,\"h\":12.0},"
    "{\"text\":\"21.6273\",\"x\":400.0,\"y\":140.0,\"w\":50.0,\"h\":12.0}"
    "]}]}";

/* ============================================================================
 * XLSX Parser Tests
 * ============================================================================ */

TEST(xlsx_null_input)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    NxXlsxStatus s = nx_xlsx_parse(NULL, 0, NULL, NULL, arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_ERR_NULL);
    sh_arena_free(arena);
}

TEST(xlsx_invalid_zip)
{
    const char *garbage = "not a zip file";
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    NxXlsxStatus s = nx_xlsx_parse(garbage, strlen(garbage), NULL, "test.xlsx",
                                    arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_ERR_ZIP);
    sh_arena_free(arena);
}

TEST(xlsx_minimal_parse)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);
    ASSERT(json != NULL);
    ASSERT(json_len > 0);

    /* Parse the output JSON to verify structure */
    SHArena *parse_arena = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    ShJsonStatus js = sh_json_parse(json, json_len, parse_arena, &root);
    ASSERT_EQ(js, SH_JSON_OK);

    /* Check top-level fields */
    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_raw"), -1), 1);

    /* Check source */
    ShJsonValue *source = sh_json_get(root, "source");
    ASSERT(source != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "filename"), ""), "minimal.xlsx");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "format"), ""), "xlsx");

    /* SHA-256 should be 64 hex chars */
    const char *sha = sh_json_as_string(sh_json_get(source, "sha256"), "");
    ASSERT_EQ(strlen(sha), 64);

    /* Check tables array */
    ShJsonValue *tables = sh_json_get(root, "tables");
    ASSERT(tables != NULL);
    ASSERT_EQ(sh_json_array_len(tables), 1);

    /* Check first table */
    ShJsonValue *t0 = sh_json_array_get(tables, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(t0, "name"), ""), "Locations");
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "index"), -1), 0);

    free(json);
    sh_arena_free(parse_arena);
    sh_arena_free(arena);
}

TEST(xlsx_headers)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_EQ(sh_json_array_len(headers), 5);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), ""), "City");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 1), ""), "Name");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 2), ""), "Address");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 3), ""), "GPS Lat");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 4), ""), "GPS Lon");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xlsx_rows)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *rows = sh_json_get(t0, "rows");

    /* Should have 2 data rows (header row excluded) */
    ASSERT_EQ(sh_json_array_len(rows), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);

    /* Row 0 (row index 1 in sheet) */
    ShJsonValue *r0 = sh_json_array_get(rows, 0);
    ShJsonValue *cells0 = sh_json_get(r0, "cells");
    ASSERT_EQ(sh_json_array_len(cells0), 5);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 0), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 1), ""), "Depot #1");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 2), ""), "Futó u. 35-37");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 3), ""), "47.4799");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 4), ""), "19.07");

    /* Row 1 (row index 2 in sheet) */
    ShJsonValue *r1 = sh_json_array_get(rows, 1);
    ShJsonValue *cells1 = sh_json_get(r1, "cells");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 0), ""), "Debrecen");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 1), ""), "Depot #2");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xlsx_sha256_matches)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    /* Compute expected SHA-256 */
    char expected[65];
    sh_sha256_hex(MINIMAL_XLSX, MINIMAL_XLSX_LEN, expected);

    /* Extract SHA from JSON */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);
    const char *actual = sh_json_as_string(
        sh_json_get(sh_json_get(root, "source"), "sha256"), "");
    ASSERT_STREQ(actual, expected);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xlsx_deterministic)
{
    /* Parse twice, verify byte-for-byte identical output */
    SHArena *arena1 = sh_arena_create(256 * 1024);
    SHArena *arena2 = sh_arena_create(256 * 1024);
    char *json1 = NULL, *json2 = NULL;
    size_t len1 = 0, len2 = 0;

    NxXlsxStatus s1 = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                     "minimal.xlsx", arena1, &json1, &len1);
    NxXlsxStatus s2 = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                     "minimal.xlsx", arena2, &json2, &len2);
    ASSERT_EQ(s1, NX_XLSX_OK);
    ASSERT_EQ(s2, NX_XLSX_OK);
    ASSERT_EQ(len1, len2);
    ASSERT(memcmp(json1, json2, len1) == 0);

    free(json1);
    free(json2);
    sh_arena_free(arena1);
    sh_arena_free(arena2);
}

TEST(xlsx_col_count)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "col_count"), -1), 5);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xlsx_unicode_strings)
{
    /* Our fixture has UTF-8 strings: Futó, Balmazújvárosi */
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "test.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *rows = sh_json_get(sh_json_array_get(sh_json_get(root, "tables"), 0), "rows");
    ShJsonValue *r1 = sh_json_array_get(rows, 1);
    ShJsonValue *cells = sh_json_get(r1, "cells");

    /* Balmazújvárosi út should be preserved */
    const char *addr = sh_json_as_string(sh_json_array_get(cells, 2), "");
    ASSERT(strstr(addr, "Balmazújvárosi") != NULL);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xlsx_status_strings)
{
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_OK)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_ZIP)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_NULL)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_NO_SHEETS)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_XML)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_ARENA)) > 0);
    ASSERT(strlen(nx_xlsx_status_str(NX_XLSX_ERR_LIMITS)) > 0);
}

TEST(xlsx_warnings_empty)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXlsxStatus s = nx_xlsx_parse(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NULL,
                                    "minimal.xlsx", arena, &json, &json_len);
    ASSERT_EQ(s, NX_XLSX_OK);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *warnings = sh_json_get(root, "warnings");
    ASSERT(warnings != NULL);
    ASSERT_EQ(sh_json_array_len(warnings), 0);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

/* ============================================================================
 * PDF Table Reconstructor Tests
 * ============================================================================ */

/* PDF parser pre-allocates MAX_TEXT_RUNS (65536) slots + row clusters,
 * needs ~5 MB of arena space */
#define PDF_ARENA_SIZE (8 * 1024 * 1024)

TEST(pdf_null_input)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);
    NxPdfStatus s = nx_pdf_extract_tables(NULL, 0, NULL, NULL, arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_ERR_NULL);
    sh_arena_free(arena);
}

TEST(pdf_invalid_json)
{
    const char *garbage = "not json at all";
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);
    NxPdfStatus s = nx_pdf_extract_tables(garbage, strlen(garbage), NULL, "test.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_ERR_JSON);
    sh_arena_free(arena);
}

TEST(pdf_empty_texts)
{
    const char *empty = "{\"pages\":[{\"page\":1,\"width\":595,\"height\":842,\"texts\":[]}]}";
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);
    NxPdfStatus s = nx_pdf_extract_tables(empty, strlen(empty), NULL, "empty.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_ERR_NO_TEXT);
    sh_arena_free(arena);
}

TEST(pdf_basic_parse)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    NxPdfStatus s = nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                           NULL, "sample.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_OK);
    ASSERT(json != NULL);
    ASSERT(json_len > 0);

    /* Parse output */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_raw"), -1), 1);

    ShJsonValue *source = sh_json_get(root, "source");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "filename"), ""), "sample.pdf");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "format"), ""), "pdf");

    /* SHA-256 should be 64 hex chars */
    const char *sha = sh_json_as_string(sh_json_get(source, "sha256"), "");
    ASSERT_EQ(strlen(sha), 64);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_headers)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                          NULL, "sample.pdf", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_EQ(sh_json_array_len(headers), 4);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), ""), "City");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 1), ""), "Name");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 2), ""), "Lat");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 3), ""), "Lon");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_rows)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                          NULL, "sample.pdf", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *rows = sh_json_get(t0, "rows");
    ASSERT_EQ(sh_json_array_len(rows), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "col_count"), -1), 4);

    /* Row 0: Budapest */
    ShJsonValue *r0 = sh_json_array_get(rows, 0);
    ShJsonValue *cells0 = sh_json_get(r0, "cells");
    ASSERT_EQ(sh_json_array_len(cells0), 4);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 0), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 1), ""), "Depot #1");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 2), ""), "47.4799");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 3), ""), "19.0700");

    /* Row 1: Debrecen */
    ShJsonValue *r1 = sh_json_array_get(rows, 1);
    ShJsonValue *cells1 = sh_json_get(r1, "cells");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 0), ""), "Debrecen");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 1), ""), "Depot #2");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 2), ""), "47.5316");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 3), ""), "21.6273");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_deterministic)
{
    SHArena *a1 = sh_arena_create(PDF_ARENA_SIZE);
    SHArena *a2 = sh_arena_create(PDF_ARENA_SIZE);
    char *j1 = NULL, *j2 = NULL;
    size_t l1 = 0, l2 = 0;

    nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                          NULL, "sample.pdf", a1, &j1, &l1);
    nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                          NULL, "sample.pdf", a2, &j2, &l2);

    ASSERT_EQ(l1, l2);
    ASSERT(memcmp(j1, j2, l1) == 0);

    free(j1);
    free(j2);
    sh_arena_free(a1);
    sh_arena_free(a2);
}

TEST(pdf_sha256_matches)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                          NULL, "sample.pdf", arena, &json, &json_len);

    char expected[65];
    sh_sha256_hex(PDF_FIXTURE, strlen(PDF_FIXTURE), expected);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);
    const char *actual = sh_json_as_string(
        sh_json_get(sh_json_get(root, "source"), "sha256"), "");
    ASSERT_STREQ(actual, expected);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_status_strings)
{
    ASSERT(strlen(nx_pdf_status_str(NX_PDF_OK)) > 0);
    ASSERT(strlen(nx_pdf_status_str(NX_PDF_ERR_NULL)) > 0);
    ASSERT(strlen(nx_pdf_status_str(NX_PDF_ERR_JSON)) > 0);
    ASSERT(strlen(nx_pdf_status_str(NX_PDF_ERR_NO_TEXT)) > 0);
    ASSERT(strlen(nx_pdf_status_str(NX_PDF_ERR_ARENA)) > 0);
}

TEST(pdf_custom_tolerance)
{
    /* Use tight row tolerance so each y-level is its own row */
    NxPdfOptions opts = { 1.0, 10.0, -1 };
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    NxPdfStatus s = nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                           &opts, "sample.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_OK);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    /* Should still get 3 rows (header + 2 data), same as default tolerance */
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_auto_detect)
{
    /* Test auto-detection: negative values should be resolved to positive */
    NxPdfOptions opts = NX_PDF_AUTO_OPTIONS;
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    NxPdfStatus s = nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                           &opts, "sample.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_OK);

    /* Auto-detected values should now be positive */
    ASSERT(opts.row_tolerance > 0);
    ASSERT(opts.col_gap_min > 0);

    /* Should still produce valid output */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);
    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ASSERT(sh_json_as_int(sh_json_get(t0, "row_count"), -1) >= 0);
    ASSERT(sh_json_as_int(sh_json_get(t0, "col_count"), -1) >= 1);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(pdf_auto_detect_null_opts)
{
    /* Test that NULL opts triggers auto-detection (same as NX_PDF_AUTO_OPTIONS) */
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    NxPdfStatus s = nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                           NULL, "sample.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_OK);
    ASSERT(json != NULL);
    ASSERT(json_len > 0);

    free(json);
    sh_arena_free(arena);
}

TEST(pdf_auto_detect_partial)
{
    /* Test partial auto-detection: only col_gap is auto, row_tol is explicit */
    NxPdfOptions opts = { 2.0, -1.0, -1 };
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(PDF_ARENA_SIZE);

    NxPdfStatus s = nx_pdf_extract_tables(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                           &opts, "sample.pdf",
                                           arena, &json, &json_len);
    ASSERT_EQ(s, NX_PDF_OK);

    /* row_tolerance should be unchanged, col_gap should be resolved */
    ASSERT(opts.row_tolerance == 2.0);
    ASSERT(opts.col_gap_min > 0);

    free(json);
    sh_arena_free(arena);
}

TEST(pdf_pipeline_integration)
{
    /* Test PDF through the full pipeline orchestrator (Stage A only, no schema) */
    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;

    NxIngestStatus s = nx_ingest(PDF_FIXTURE, strlen(PDF_FIXTURE),
                                  NX_FORMAT_PDF_JSON, "sample.pdf",
                                  NULL, 0,
                                  &raw, &raw_len, &canon, &canon_len);
    ASSERT_EQ(s, NX_INGEST_OK);
    ASSERT(raw != NULL);
    ASSERT(raw_len > 0);

    /* Verify raw output has nx_raw structure */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(raw, raw_len, pa, &root);
    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_raw"), -1), 1);

    free(raw);
    sh_arena_free(pa);
}

/* ============================================================================
 * CSV Parser Tests
 * ============================================================================ */

static const char CSV_FIXTURE[] =
    "City,Name,Address,GPS Lat,GPS Lon\n"
    "Budapest,Depot #1,\"Futó u. 35-37\",47.4799,19.07\n"
    "Debrecen,Depot #2,\"Balmazújvárosi út 11\",47.5316,21.6273\n";

static const char CSV_TSV_FIXTURE[] =
    "City\tName\tLat\tLon\n"
    "Budapest\tDepot #1\t47.4799\t19.07\n"
    "Debrecen\tDepot #2\t47.5316\t21.6273\n";

static const char CSV_NO_HEADER_FIXTURE[] =
    "Budapest,Depot #1,47.4799,19.07\n"
    "Debrecen,Depot #2,47.5316,21.6273\n";

TEST(csv_null_input)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    NxCsvStatus s = nx_csv_parse(NULL, 0, NULL, NULL, NULL, arena, &json, &json_len);
    ASSERT_EQ(s, NX_CSV_ERR_NULL);
    sh_arena_free(arena);
}

TEST(csv_empty_input)
{
    const char *empty = "";
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    NxCsvStatus s = nx_csv_parse(empty, 0, NULL, NULL, "test.csv",
                                  arena, &json, &json_len);
    ASSERT_EQ(s, NX_CSV_ERR_NO_DATA);
    sh_arena_free(arena);
}

TEST(csv_basic_parse)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxCsvStatus s = nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                                  NULL, NULL, "locations.csv",
                                  arena, &json, &json_len);
    ASSERT_EQ(s, NX_CSV_OK);
    ASSERT(json != NULL);
    ASSERT(json_len > 0);

    /* Parse output */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_raw"), -1), 1);

    ShJsonValue *source = sh_json_get(root, "source");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "filename"), ""), "locations.csv");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "format"), ""), "csv");

    /* SHA-256 should be 64 hex chars */
    const char *sha = sh_json_as_string(sh_json_get(source, "sha256"), "");
    ASSERT_EQ(strlen(sha), 64);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_headers)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_EQ(sh_json_array_len(headers), 5);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), ""), "City");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 1), ""), "Name");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 2), ""), "Address");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 3), ""), "GPS Lat");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 4), ""), "GPS Lon");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_rows)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *rows = sh_json_get(t0, "rows");
    ASSERT_EQ(sh_json_array_len(rows), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "col_count"), -1), 5);

    /* Row 0: Budapest */
    ShJsonValue *r0 = sh_json_array_get(rows, 0);
    ShJsonValue *cells0 = sh_json_get(r0, "cells");
    ASSERT_EQ(sh_json_array_len(cells0), 5);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 0), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 1), ""), "Depot #1");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 2), ""), "Futó u. 35-37");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 3), ""), "47.4799");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 4), ""), "19.07");

    /* Row 1: Debrecen */
    ShJsonValue *r1 = sh_json_array_get(rows, 1);
    ShJsonValue *cells1 = sh_json_get(r1, "cells");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 0), ""), "Debrecen");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells1, 1), ""), "Depot #2");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_sha256_matches)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena, &json, &json_len);

    char expected[65];
    sh_sha256_hex(CSV_FIXTURE, strlen(CSV_FIXTURE), expected);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);
    const char *actual = sh_json_as_string(
        sh_json_get(sh_json_get(root, "source"), "sha256"), "");
    ASSERT_STREQ(actual, expected);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_deterministic)
{
    SHArena *arena1 = sh_arena_create(256 * 1024);
    SHArena *arena2 = sh_arena_create(256 * 1024);
    char *json1 = NULL, *json2 = NULL;
    size_t len1 = 0, len2 = 0;

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena1, &json1, &len1);
    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena2, &json2, &len2);

    ASSERT_EQ(len1, len2);
    ASSERT(memcmp(json1, json2, len1) == 0);

    free(json1);
    free(json2);
    sh_arena_free(arena1);
    sh_arena_free(arena2);
}

TEST(csv_status_strings)
{
    ASSERT(strlen(nx_csv_status_str(NX_CSV_OK)) > 0);
    ASSERT(strlen(nx_csv_status_str(NX_CSV_ERR_NULL)) > 0);
    ASSERT(strlen(nx_csv_status_str(NX_CSV_ERR_PARSE)) > 0);
    ASSERT(strlen(nx_csv_status_str(NX_CSV_ERR_NO_DATA)) > 0);
    ASSERT(strlen(nx_csv_status_str(NX_CSV_ERR_ARENA)) > 0);
}

TEST(csv_tsv_auto_detect)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxCsvStatus s = nx_csv_parse(CSV_TSV_FIXTURE, strlen(CSV_TSV_FIXTURE),
                                  NULL, NULL, "data.tsv",
                                  arena, &json, &json_len);
    ASSERT_EQ(s, NX_CSV_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_EQ(sh_json_array_len(headers), 4);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), ""), "City");
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_no_header)
{
    ShCsvOpts opts;
    sh_csv_opts_default(&opts);
    opts.has_header = 0;

    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxCsvStatus s = nx_csv_parse(CSV_NO_HEADER_FIXTURE,
                                  strlen(CSV_NO_HEADER_FIXTURE),
                                  &opts, NULL, "noheader.csv",
                                  arena, &json, &json_len);
    ASSERT_EQ(s, NX_CSV_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *t0 = sh_json_array_get(sh_json_get(root, "tables"), 0);

    /* No header → headers should be empty strings */
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_EQ(sh_json_array_len(headers), 4);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), "x"), "");

    /* All rows are data rows */
    ASSERT_EQ(sh_json_as_int(sh_json_get(t0, "row_count"), -1), 2);

    ShJsonValue *rows = sh_json_get(t0, "rows");
    ShJsonValue *r0 = sh_json_array_get(rows, 0);
    ShJsonValue *cells0 = sh_json_get(r0, "cells");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(cells0, 0), ""), "Budapest");

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_unicode_strings)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *rows = sh_json_get(sh_json_array_get(sh_json_get(root, "tables"), 0), "rows");
    ShJsonValue *r1 = sh_json_array_get(rows, 1);
    ShJsonValue *cells = sh_json_get(r1, "cells");
    const char *addr = sh_json_as_string(sh_json_array_get(cells, 2), "");
    ASSERT(strstr(addr, "Balmazújvárosi") != NULL);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_warnings_empty)
{
    char *json = NULL;
    size_t json_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    nx_csv_parse(CSV_FIXTURE, strlen(CSV_FIXTURE),
                  NULL, NULL, "test.csv", arena, &json, &json_len);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(json, json_len, pa, &root);

    ShJsonValue *warnings = sh_json_get(root, "warnings");
    ASSERT(warnings != NULL);
    ASSERT_EQ(sh_json_array_len(warnings), 0);

    free(json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(csv_pipeline_integration)
{
    /* Test CSV through the full pipeline orchestrator */
    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;

    NxIngestStatus s = nx_ingest(CSV_FIXTURE, strlen(CSV_FIXTURE),
                                  NX_FORMAT_CSV, "data.csv",
                                  NULL, 0,
                                  &raw, &raw_len, &canon, &canon_len);
    ASSERT_EQ(s, NX_INGEST_OK);
    ASSERT(raw != NULL);
    ASSERT(raw_len > 0);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(raw, raw_len, pa, &root);
    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_raw"), -1), 1);

    ShJsonValue *source = sh_json_get(root, "source");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(source, "format"), ""), "csv");

    free(raw);
    sh_arena_free(pa);
}

/* ============================================================================
 * Golden Tests (real-world files, skipped if missing)
 * ============================================================================ */

#define GOLDEN_DIR "tests/golden/"

/* Helper: read file into malloc'd buffer, returns NULL if missing */
static char *read_golden_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nr != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* Macro for golden tests that skip when file is missing */
static int golden_skipped = 0;
#define RUN_GOLDEN(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    int _skip = 0; \
    test_golden_##name(&_skip); \
    if (_skip) { printf("[SKIP]\n"); golden_skipped++; } \
    else { tests_run++; tests_passed++; printf("[PASS]\n"); } \
} while (0)

/* Golden: XLSX01 depot file parses correctly */
static void test_golden_xlsx01_parse(int *skip)
{
    size_t len = 0;
    char *data = read_golden_file(GOLDEN_DIR "XLSX01.xlsx", &len);
    if (!data) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *raw = NULL;
    size_t raw_len = 0;

    NxXlsxStatus st = nx_xlsx_parse(data, len, NULL, "XLSX01.xlsx",
                                     arena, &raw, &raw_len);
    ASSERT_EQ(st, NX_XLSX_OK);
    ASSERT(raw != NULL);
    ASSERT(raw_len > 0);

    /* Parse and verify structure */
    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    ShJsonStatus js = sh_json_parse(raw, raw_len, pa, &root);
    ASSERT_EQ(js, SH_JSON_OK);
    ASSERT(root != NULL);

    ShJsonValue *tables = sh_json_get(root, "tables");
    ASSERT(tables != NULL);
    ASSERT_EQ((int)sh_json_array_len(tables), 1);

    ShJsonValue *t0 = sh_json_array_get(tables, 0);
    ASSERT_EQ((int)sh_json_as_double(sh_json_get(t0, "row_count"), 0), 7);
    ASSERT_EQ((int)sh_json_as_double(sh_json_get(t0, "col_count"), 0), 7);

    /* Verify first header is "Park neve" */
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(headers, 0), ""), "Park neve");

    free(data);
    free(raw);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

/* Golden: XLSX01 canonical transform produces valid depot records */
static void test_golden_xlsx01_canonical(int *skip)
{
    size_t data_len = 0;
    char *data = read_golden_file(GOLDEN_DIR "XLSX01.xlsx", &data_len);
    if (!data) { *skip = 1; return; }

    size_t schema_len = 0;
    char *schema = read_golden_file("schemas/gls-hu-depots-v1.json", &schema_len);
    if (!schema) { free(data); *skip = 1; return; }

    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;

    NxIngestStatus st = nx_ingest(data, data_len, NX_FORMAT_XLSX, "XLSX01.xlsx",
                                   schema, schema_len,
                                   &raw, &raw_len, &canon, &canon_len);
    ASSERT_EQ(st, NX_INGEST_OK);
    ASSERT(canon != NULL);
    ASSERT(canon_len > 0);

    /* Parse canonical output */
    SHArena *arena = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    ShJsonStatus js = sh_json_parse(canon, canon_len, arena, &root);
    ASSERT_EQ(js, SH_JSON_OK);
    ASSERT_EQ((int)sh_json_as_double(sh_json_get(root, "record_count"), 0), 7);

    /* Check first record has valid lat/lon in Hungary */
    ShJsonValue *records = sh_json_get(root, "records");
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    double lat = sh_json_as_double(sh_json_get(r0, "lat"), 0);
    double lon = sh_json_as_double(sh_json_get(r0, "lon"), 0);
    ASSERT(lat > 45.0 && lat < 49.0);  /* Hungary lat range */
    ASSERT(lon > 16.0 && lon < 23.0);  /* Hungary lon range */

    /* Check derived fields */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "facility_type"), ""), "depot");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "country"), ""), "HU");

    free(data);
    free(schema);
    free(raw);
    free(canon);
    sh_arena_free(arena);
}

/* Golden: PDF01 automata file extracts table rows */
static void test_golden_pdf01_raw(int *skip)
{
    size_t len = 0;
    char *data = read_golden_file(GOLDEN_DIR "PDF01.pdf", &len);
    if (!data) { *skip = 1; return; }

    /* PDF01 is raw PDF - it needs sh_pdf2struc which is in nx_pipeline,
       not in the library. Test the raw JSON output file instead. */
    free(data);

    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF01_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    ShJsonValue *root = NULL;
    ShJsonStatus js = sh_json_parse(raw, raw_len, arena, &root);
    ASSERT_EQ(js, SH_JSON_OK);

    ShJsonValue *tables = sh_json_get(root, "tables");
    ASSERT(tables != NULL);
    ASSERT(sh_json_array_len(tables) >= 1);

    ShJsonValue *t0 = sh_json_array_get(tables, 0);
    int row_count = (int)sh_json_as_double(sh_json_get(t0, "row_count"), 0);
    int col_count = (int)sh_json_as_double(sh_json_get(t0, "col_count"), 0);

    /* PDF01 should have hundreds of rows (parcel automata list) */
    ASSERT(row_count > 100);
    ASSERT(col_count > 10);

    /* Verify headers include GPS-related columns */
    ShJsonValue *headers = sh_json_get(t0, "headers");
    ASSERT(headers != NULL);
    int found_gps = 0;
    for (size_t i = 0; i < sh_json_array_len(headers); i++) {
        const char *h = sh_json_as_string(sh_json_array_get(headers, i), "");
        if (strstr(h, "GPS") != NULL) found_gps = 1;
    }
    ASSERT(found_gps);

    free(raw);
    sh_arena_free(arena);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nNexus Ingestion Tests:\n");

    printf("\n  XLSX Parser:\n");
    RUN_TEST(xlsx_null_input);
    RUN_TEST(xlsx_invalid_zip);
    RUN_TEST(xlsx_minimal_parse);
    RUN_TEST(xlsx_headers);
    RUN_TEST(xlsx_rows);
    RUN_TEST(xlsx_sha256_matches);
    RUN_TEST(xlsx_deterministic);
    RUN_TEST(xlsx_col_count);
    RUN_TEST(xlsx_unicode_strings);
    RUN_TEST(xlsx_status_strings);
    RUN_TEST(xlsx_warnings_empty);

    printf("\n  PDF Table Reconstructor:\n");
    RUN_TEST(pdf_null_input);
    RUN_TEST(pdf_invalid_json);
    RUN_TEST(pdf_empty_texts);
    RUN_TEST(pdf_basic_parse);
    RUN_TEST(pdf_headers);
    RUN_TEST(pdf_rows);
    RUN_TEST(pdf_deterministic);
    RUN_TEST(pdf_sha256_matches);
    RUN_TEST(pdf_status_strings);
    RUN_TEST(pdf_custom_tolerance);
    RUN_TEST(pdf_auto_detect);
    RUN_TEST(pdf_auto_detect_null_opts);
    RUN_TEST(pdf_auto_detect_partial);
    RUN_TEST(pdf_pipeline_integration);

    printf("\n  CSV Parser:\n");
    RUN_TEST(csv_null_input);
    RUN_TEST(csv_empty_input);
    RUN_TEST(csv_basic_parse);
    RUN_TEST(csv_headers);
    RUN_TEST(csv_rows);
    RUN_TEST(csv_sha256_matches);
    RUN_TEST(csv_deterministic);
    RUN_TEST(csv_status_strings);
    RUN_TEST(csv_tsv_auto_detect);
    RUN_TEST(csv_no_header);
    RUN_TEST(csv_unicode_strings);
    RUN_TEST(csv_warnings_empty);
    RUN_TEST(csv_pipeline_integration);

    printf("\n  Golden Tests (real-world files):\n");
    RUN_GOLDEN(xlsx01_parse);
    RUN_GOLDEN(xlsx01_canonical);
    RUN_GOLDEN(pdf01_raw);

    printf("\nNexus Ingestion: %d passed, %d total", tests_passed, tests_run);
    if (golden_skipped > 0) printf(" (%d golden skipped)", golden_skipped);
    printf("\n");
    return tests_passed == tests_run ? 0 : 1;
}
