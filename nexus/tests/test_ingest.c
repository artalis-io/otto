/*
 * test_ingest.c - Tests for Nexus ingestion pipeline
 */

#include "nx_xlsx.h"
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
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nNexus Ingestion Tests:\n");

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

    printf("\nNexus Ingestion: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
