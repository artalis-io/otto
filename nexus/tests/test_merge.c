/*
 * test_merge.c - Tests for continuation row merging
 */

#include "nx_merge.h"
#include "nx_xform.h"
#include "sh_json.h"
#include "sh_arena.h"
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
#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n    at %s:%d\n", \
               _b, _a, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * Golden file helpers
 * ============================================================================ */

#define GOLDEN_DIR "tests/golden/"

static char *read_golden_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_len) *out_len = nread;
    return buf;
}

static int golden_skipped = 0;
#define RUN_GOLDEN(name) do { \
    printf("  %-55s ", #name); \
    fflush(stdout); \
    int _skip = 0; \
    test_golden_##name(&_skip); \
    if (_skip) { printf("[SKIP]\n"); golden_skipped++; } \
    else { tests_run++; tests_passed++; printf("[PASS]\n"); } \
} while (0)

/* ============================================================================
 * Helper: Parse merged JSON and extract data
 * ============================================================================ */

static ShJsonValue *parse_json(const char *json, size_t len, SHArena *arena)
{
    ShJsonValue *root = NULL;
    ShJsonStatus st = sh_json_parse(json, len, arena, &root);
    if (st != SH_JSON_OK) return NULL;
    return root;
}

static size_t get_merged_row_count(const char *json, size_t len, SHArena *arena)
{
    ShJsonValue *root = parse_json(json, len, arena);
    if (!root) return 0;
    ShJsonValue *tables = sh_json_get(root, "tables");
    if (!tables || sh_json_array_len(tables) == 0) return 0;
    ShJsonValue *table = sh_json_array_get(tables, 0);
    ShJsonValue *rows = sh_json_get(table, "rows");
    return rows ? sh_json_array_len(rows) : 0;
}

static const char *get_cell(ShJsonValue *root, size_t row_idx, size_t col_idx)
{
    ShJsonValue *tables = sh_json_get(root, "tables");
    ShJsonValue *table = sh_json_array_get(tables, 0);
    ShJsonValue *rows = sh_json_get(table, "rows");
    ShJsonValue *row = sh_json_array_get(rows, row_idx);
    ShJsonValue *cells = sh_json_get(row, "cells");
    return sh_json_as_string(sh_json_array_get(cells, col_idx), "");
}

/* ============================================================================
 * Simple raw JSON builder for tests
 * ============================================================================ */

/* Minimal schema with row_merge config */
static const char *SCHEMA_BASIC =
    "{\"nx_schema\":2,\"version\":\"test\","
    "\"row_merge\":{\"key_columns\":[0,1,2],\"separator\":\" \"},"
    "\"columns\":[]}";

static const char *SCHEMA_CUSTOM_SEP =
    "{\"nx_schema\":2,\"version\":\"test\","
    "\"row_merge\":{\"key_columns\":[0,1],\"separator\":\", \"},"
    "\"columns\":[]}";

static const char *SCHEMA_DEFAULT_SEP =
    "{\"nx_schema\":2,\"version\":\"test\","
    "\"row_merge\":{\"key_columns\":[0]},"
    "\"columns\":[]}";

static const char *SCHEMA_WITH_STRIP =
    "{\"nx_schema\":2,\"version\":\"test\","
    "\"row_merge\":{\"key_columns\":[0,1,2],\"separator\":\" \","
    "\"strip_pattern\":\"Page Header\"},"
    "\"columns\":[]}";

static const char *SCHEMA_NO_MERGE =
    "{\"nx_schema\":2,\"version\":\"test\","
    "\"columns\":[]}";

/* ============================================================================
 * Unit Tests: Error Cases
 * ============================================================================ */

TEST(null_input)
{
    ASSERT_EQ(nx_merge_rows(NULL, 0, NULL, 0, NULL, NULL, NULL), NX_MERGE_ERR_NULL);
}

TEST(invalid_json)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows("not json", 8,
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_ERR_JSON);
    sh_arena_free(arena);
}

TEST(no_config_passthrough)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    const char *raw = "{\"tables\":[{\"rows\":[{\"row\":1,\"cells\":[\"a\"]}]}]}";
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_NO_MERGE, strlen(SCHEMA_NO_MERGE),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out == NULL); /* No-op signal */
    sh_arena_free(arena);
}

TEST(status_strings)
{
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_OK), "OK");
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_ERR_NULL), "NULL input");
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_ERR_JSON), "Invalid JSON");
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_ERR_SCHEMA), "Invalid schema");
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_ERR_NO_TABLE), "No tables in raw JSON");
    ASSERT_STREQ(nx_merge_status_str(NX_MERGE_ERR_ARENA), "Arena allocation failure");
}

/* ============================================================================
 * Unit Tests: Basic Merging
 * ============================================================================ */

TEST(basic_single_continuation)
{
    /* Row 1: data row, Row 2: continuation (cols 0-2 empty) */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"source\":{\"filename\":\"test.pdf\",\"sha256\":\"abc\",\"format\":\"pdf\"},"
        "\"tables\":[{\"name\":\"Page1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Hours\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1234\",\"Budapest\",\"Shop A\",\"H-P: 8-17\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"Szo: 8-12\"]}"
        "],\"row_count\":2,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    size_t nrows = get_merged_row_count(out, out_len, arena2);
    ASSERT_EQ(nrows, (size_t)1);

    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 0), "1234");
    ASSERT_STREQ(get_cell(root, 0, 1), "Budapest");
    ASSERT_STREQ(get_cell(root, 0, 2), "Shop A");
    ASSERT_STREQ(get_cell(root, 0, 3), "H-P: 8-17 Szo: 8-12");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(multi_continuation)
{
    /* One data row followed by two continuation rows */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Hours\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1234\",\"City\",\"Shop\",\"Line1\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"Line2\"]},"
        "{\"row\":3,\"cells\":[\"\",\"\",\"\",\"Line3\"]}"
        "],\"row_count\":3,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ASSERT_EQ(get_merged_row_count(out, out_len, arena2), (size_t)1);

    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 3), "Line1 Line2 Line3");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(no_continuations)
{
    /* All rows have non-empty key columns — passthrough */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1111\",\"A\",\"X\"]},"
        "{\"row\":2,\"cells\":[\"2222\",\"B\",\"Y\"]},"
        "{\"row\":3,\"cells\":[\"3333\",\"C\",\"Z\"]}"
        "],\"row_count\":3,\"col_count\":3}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ASSERT_EQ(get_merged_row_count(out, out_len, arena2), (size_t)3);

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(custom_separator)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"A\",\"B\",\"C\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"X\",\"Y\",\"val1\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"val2\"]}"
        "],\"row_count\":2,\"col_count\":3}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_CUSTOM_SEP, strlen(SCHEMA_CUSTOM_SEP),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 2), "val1, val2");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(default_separator)
{
    /* No explicit separator → defaults to " " */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"Key\",\"Val\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"A\",\"hello\"]},"
        "{\"row\":2,\"cells\":[\"\",\"world\"]}"
        "],\"row_count\":2,\"col_count\":2}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_DEFAULT_SEP, strlen(SCHEMA_DEFAULT_SEP),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 1), "hello world");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Unit Tests: Strip Pattern
 * ============================================================================ */

TEST(strip_pattern)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"A\",\"B\",\"C\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1\",\"Data\",\"X\"]},"
        "{\"row\":2,\"cells\":[\"\",\"Page Header Repeat\",\"\"]},"
        "{\"row\":3,\"cells\":[\"2\",\"More\",\"Y\"]}"
        "],\"row_count\":3,\"col_count\":3}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_WITH_STRIP, strlen(SCHEMA_WITH_STRIP),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ASSERT_EQ(get_merged_row_count(out, out_len, arena2), (size_t)2);

    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 1), "Data");
    ASSERT_STREQ(get_cell(root, 1, 1), "More");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Unit Tests: Edge Cases
 * ============================================================================ */

TEST(empty_continuation_cells)
{
    /* Continuation has empty cells — should NOT overwrite parent */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Hours\",\"Notes\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1234\",\"City\",\"Shop\",\"H-P: 8-17\",\"note1\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"Szo: 8-12\",\"\"]}"
        "],\"row_count\":2,\"col_count\":5}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    /* Hours merged */
    ASSERT_STREQ(get_cell(root, 0, 3), "H-P: 8-17 Szo: 8-12");
    /* Notes NOT changed (continuation cell was empty) */
    ASSERT_STREQ(get_cell(root, 0, 4), "note1");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(first_row_continuation)
{
    /* Continuation before first data row — should be discarded */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Val\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"\",\"\",\"\",\"orphan\"]},"
        "{\"row\":2,\"cells\":[\"1234\",\"City\",\"Shop\",\"data\"]}"
        "],\"row_count\":2,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, strlen(raw),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ASSERT_EQ(get_merged_row_count(out, out_len, arena2), (size_t)1);

    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 0), "1234");
    ASSERT_STREQ(get_cell(root, 0, 3), "data");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(row_count_updated)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Val\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1111\",\"A\",\"X\",\"v1\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"extra\"]},"
        "{\"row\":3,\"cells\":[\"2222\",\"B\",\"Y\",\"v2\"]}"
        "],\"row_count\":3,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    nx_merge_rows(raw, strlen(raw),
                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                  arena, &out, &out_len);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ShJsonValue *table = sh_json_array_get(sh_json_get(root, "tables"), 0);
    int rc = (int)sh_json_as_int(sh_json_get(table, "row_count"), -1);
    ASSERT_EQ(rc, 2);

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(row_numbers_sequential)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"Val\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1111\",\"A\",\"X\",\"v1\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"cont\"]},"
        "{\"row\":3,\"cells\":[\"2222\",\"B\",\"Y\",\"v2\"]},"
        "{\"row\":4,\"cells\":[\"3333\",\"C\",\"Z\",\"v3\"]}"
        "],\"row_count\":4,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    nx_merge_rows(raw, strlen(raw),
                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                  arena, &out, &out_len);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ShJsonValue *rows = sh_json_get(sh_json_array_get(sh_json_get(root, "tables"), 0), "rows");

    ASSERT_EQ(sh_json_array_len(rows), (size_t)3);
    ASSERT_EQ((int)sh_json_as_int(sh_json_get(sh_json_array_get(rows, 0), "row"), 0), 1);
    ASSERT_EQ((int)sh_json_as_int(sh_json_get(sh_json_array_get(rows, 1), "row"), 0), 2);
    ASSERT_EQ((int)sh_json_as_int(sh_json_get(sh_json_array_get(rows, 2), "row"), 0), 3);

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(preserves_source_metadata)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"source\":{\"filename\":\"test.pdf\",\"sha256\":\"deadbeef\",\"format\":\"pdf\"},"
        "\"tables\":[{\"name\":\"Page1\",\"index\":0,"
        "\"headers\":[\"A\",\"B\",\"C\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1\",\"2\",\"3\"]}"
        "],\"row_count\":1,\"col_count\":3}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    nx_merge_rows(raw, strlen(raw),
                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                  arena, &out, &out_len);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);

    ASSERT_EQ((int)sh_json_as_int(sh_json_get(root, "nx_raw"), 0), 1);

    ShJsonValue *src = sh_json_get(root, "source");
    ASSERT(src != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(src, "filename"), ""), "test.pdf");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(src, "sha256"), ""), "deadbeef");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(src, "format"), ""), "pdf");

    ShJsonValue *table = sh_json_array_get(sh_json_get(root, "tables"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(table, "name"), ""), "Page1");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

TEST(mixed_data_and_continuations)
{
    /* data, cont, data, cont, cont, data */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"nx_raw\":1,"
        "\"tables\":[{\"name\":\"P1\",\"index\":0,"
        "\"headers\":[\"ZIP\",\"City\",\"Name\",\"H\"],"
        "\"header_row\":0,"
        "\"rows\":["
        "{\"row\":1,\"cells\":[\"1111\",\"A\",\"S1\",\"a\"]},"
        "{\"row\":2,\"cells\":[\"\",\"\",\"\",\"b\"]},"
        "{\"row\":3,\"cells\":[\"2222\",\"B\",\"S2\",\"c\"]},"
        "{\"row\":4,\"cells\":[\"\",\"\",\"\",\"d\"]},"
        "{\"row\":5,\"cells\":[\"\",\"\",\"\",\"e\"]},"
        "{\"row\":6,\"cells\":[\"3333\",\"C\",\"S3\",\"f\"]}"
        "],\"row_count\":6,\"col_count\":4}],"
        "\"warnings\":[]}";

    char *out = NULL; size_t out_len = 0;
    nx_merge_rows(raw, strlen(raw),
                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                  arena, &out, &out_len);

    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ASSERT_EQ(get_merged_row_count(out, out_len, arena2), (size_t)3);

    ShJsonValue *root = parse_json(out, out_len, arena2);
    ASSERT_STREQ(get_cell(root, 0, 3), "a b");
    ASSERT_STREQ(get_cell(root, 1, 3), "c d e");
    ASSERT_STREQ(get_cell(root, 2, 3), "f");

    free(out);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Golden Tests: PDF02 (GLS Hungary PuDo)
 * ============================================================================ */

static const char *SCHEMA_PDF02 =
    "{\"nx_schema\":2,\"version\":\"gls-hu-pudo-v2\","
    "\"row_merge\":{\"key_columns\":[0,1,2],\"separator\":\" \","
    "\"strip_pattern\":\"GLS CsomagPontok\"},"
    "\"columns\":[]}";

static void test_golden_pdf02_merge_row_count(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(16 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, raw_len,
                                      SCHEMA_PDF02, strlen(SCHEMA_PDF02),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);
    ASSERT(out != NULL);

    SHArena *arena2 = sh_arena_create(16 * 1024 * 1024);
    size_t nrows = get_merged_row_count(out, out_len, arena2);

    /* 1074 raw - 195 continuation - 21 header = 858 */
    ASSERT(nrows > 800 && nrows < 900);

    /* Verify row_count field matches actual rows */
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ShJsonValue *table = sh_json_array_get(sh_json_get(root, "tables"), 0);
    int rc = (int)sh_json_as_int(sh_json_get(table, "row_count"), -1);
    ASSERT_EQ((size_t)rc, nrows);

    free(out);
    free(raw);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

static void test_golden_pdf02_merge_hours_joined(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(16 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, raw_len,
                                      SCHEMA_PDF02, strlen(SCHEMA_PDF02),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);

    /* Find the Ács row (KoKo Autósbolt) - should have merged hours */
    SHArena *arena2 = sh_arena_create(16 * 1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ShJsonValue *rows = sh_json_get(sh_json_array_get(sh_json_get(root, "tables"), 0), "rows");
    size_t nrows = sh_json_array_len(rows);

    bool found = false;
    for (size_t i = 0; i < nrows; i++) {
        ShJsonValue *row = sh_json_array_get(rows, i);
        ShJsonValue *cells = sh_json_get(row, "cells");
        const char *city = sh_json_as_string(sh_json_array_get(cells, 1), "");
        const char *name = sh_json_as_string(sh_json_array_get(cells, 2), "");
        if (strcmp(city, "Ács") == 0 && strstr(name, "KoKo") != NULL) {
            const char *hours = sh_json_as_string(sh_json_array_get(cells, 4), "");
            /* Hours should contain both parts merged: "...E:12:00- 13:00" */
            ASSERT(strstr(hours, "E:12:00-") != NULL);
            ASSERT(strstr(hours, "13:00") != NULL);
            found = true;
            break;
        }
    }
    ASSERT(found);

    free(out);
    free(raw);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

static void test_golden_pdf02_merge_notes_joined(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(16 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxMergeStatus st = nx_merge_rows(raw, raw_len,
                                      SCHEMA_PDF02, strlen(SCHEMA_PDF02),
                                      arena, &out, &out_len);
    ASSERT_EQ(st, NX_MERGE_OK);

    /* Find Abaújszántó row - notes should merge "BANKKÁRTYÁS FIZETÉS IS" + "CSAK CSOMAGÁTADÁS" */
    SHArena *arena2 = sh_arena_create(16 * 1024 * 1024);
    ShJsonValue *root = parse_json(out, out_len, arena2);
    ShJsonValue *rows = sh_json_get(sh_json_array_get(sh_json_get(root, "tables"), 0), "rows");
    size_t nrows = sh_json_array_len(rows);

    bool found = false;
    for (size_t i = 0; i < nrows; i++) {
        ShJsonValue *row = sh_json_array_get(rows, i);
        ShJsonValue *cells = sh_json_get(row, "cells");
        const char *city = sh_json_as_string(sh_json_array_get(cells, 1), "");
        if (strcmp(city, "Abaújszántó") == 0) {
            const char *notes = sh_json_as_string(sh_json_array_get(cells, 6), "");
            ASSERT(strstr(notes, "BANKKÁRTYÁS FIZETÉS IS") != NULL);
            ASSERT(strstr(notes, "CSAK CSOMAGÁTADÁS") != NULL);
            found = true;
            break;
        }
    }
    ASSERT(found);

    free(out);
    free(raw);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

static void test_golden_pdf02_merge_transform_e2e(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    /* Read the real pudo-v2 schema */
    size_t schema_len = 0;
    char *schema = read_golden_file("schemas/gls-hu-pudo-v2.json", &schema_len);
    if (!schema) { free(raw); *skip = 1; return; }

    /* Merge first */
    SHArena *arena = sh_arena_create(16 * 1024 * 1024);
    char *merged = NULL; size_t merged_len = 0;
    NxMergeStatus ms = nx_merge_rows(raw, raw_len,
                                      schema, schema_len,
                                      arena, &merged, &merged_len);
    ASSERT_EQ(ms, NX_MERGE_OK);

    /* If schema has row_merge, use merged; otherwise use raw */
    const char *xform_input = merged ? merged : raw;
    size_t xform_input_len = merged ? merged_len : raw_len;

    /* Transform */
    SHArena *arena_b = sh_arena_create(16 * 1024 * 1024);
    char *canon = NULL; size_t canon_len = 0;
    NxXformStatus xst = nx_xform_apply(xform_input, xform_input_len,
                                        schema, schema_len,
                                        arena_b, &canon, &canon_len);
    ASSERT_EQ(xst, NX_XFORM_OK);
    ASSERT(canon != NULL);

    /* Verify pipeline completed without crash */
    SHArena *arena2 = sh_arena_create(4 * 1024 * 1024);
    ShJsonValue *result = parse_json(canon, canon_len, arena2);
    ASSERT(result != NULL);

    /* Canonical output should have nx_canonical marker */
    ASSERT_EQ((int)sh_json_as_int(sh_json_get(result, "nx_canonical"), 0), 1);

    /* Audit trail should exist */
    ShJsonValue *audit = sh_json_get(result, "audit");
    ASSERT(audit != NULL);
    int rows_proc = (int)sh_json_as_int(sh_json_get(audit, "rows_processed"), -1);
    ASSERT(rows_proc > 0);

    free(merged);
    free(canon);
    free(raw);
    free(schema);
    sh_arena_free(arena);
    sh_arena_free(arena_b);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nRow Merge Tests:\n\n");

    printf("  Error Cases:\n");
    RUN_TEST(null_input);
    RUN_TEST(invalid_json);
    RUN_TEST(no_config_passthrough);
    RUN_TEST(status_strings);

    printf("\n  Basic Merging:\n");
    RUN_TEST(basic_single_continuation);
    RUN_TEST(multi_continuation);
    RUN_TEST(no_continuations);
    RUN_TEST(custom_separator);
    RUN_TEST(default_separator);

    printf("\n  Strip Pattern:\n");
    RUN_TEST(strip_pattern);

    printf("\n  Edge Cases:\n");
    RUN_TEST(empty_continuation_cells);
    RUN_TEST(first_row_continuation);
    RUN_TEST(row_count_updated);
    RUN_TEST(row_numbers_sequential);
    RUN_TEST(preserves_source_metadata);
    RUN_TEST(mixed_data_and_continuations);

    printf("\n  Golden Tests (PDF02):\n");
    RUN_GOLDEN(pdf02_merge_row_count);
    RUN_GOLDEN(pdf02_merge_hours_joined);
    RUN_GOLDEN(pdf02_merge_notes_joined);
    RUN_GOLDEN(pdf02_merge_transform_e2e);

    printf("\nRow Merge: %d passed, %d total", tests_passed, tests_run);
    if (golden_skipped > 0) printf(" (%d golden skipped)", golden_skipped);
    printf("\n");
    return tests_passed == tests_run ? 0 : 1;
}
