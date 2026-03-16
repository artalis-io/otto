/*
 * test_validate.c - Tests for semantic validation engine (Stage X)
 */

#include "nx_validate.h"
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
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))
#define ASSERT_STREQ(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("[FAIL]\n    Expected: \"%s\"\n    Got:      \"%s\"\n    at %s:%d\n", \
               _b, _a, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/* ============================================================================
 * Test Fixtures
 * ============================================================================ */

static const char *CANONICAL_JSON =
    "{"
    "\"nx_canonical\":1,"
    "\"schema_version\":\"test-v1\","
    "\"source_sha256\":\"abc123\","
    "\"output_type\":\"facility\","
    "\"records\":["
        "{\"id\":\"loc-1\",\"lat\":47.5,\"lon\":19.0,\"zip\":\"1234\"},"
        "{\"id\":\"loc-2\",\"lat\":48.0,\"lon\":20.0,\"zip\":\"5678\"},"
        "{\"id\":\"loc-1\",\"lat\":47.5,\"lon\":19.0,\"zip\":\"1234\"}" /* Duplicate */
    "],"
    "\"record_count\":3,"
    "\"audit\":{"
        "\"rows_processed\":3,"
        "\"rows_accepted\":3,"
        "\"rows_rejected\":0,"
        "\"rejections\":[]"
    "}"
    "}";

static const char *SCHEMA_NO_VALIDATION =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\""
    "}";

static const char *SCHEMA_GEO_BOUNDS =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\","
    "\"validate\":["
        "{\"type\":\"geo_bounds\",\"lat_field\":\"lat\",\"lon_field\":\"lon\","
         "\"bounds\":{\"min_lat\":45.0,\"max_lat\":49.0,\"min_lon\":16.0,\"max_lon\":23.0},"
         "\"severity\":\"error\"}"
    "]"
    "}";

static const char *SCHEMA_FORMAT =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\","
    "\"validate\":["
        "{\"type\":\"format\",\"field\":\"zip\",\"pattern\":\"^[0-9]{4}$\","
         "\"message\":\"Must be 4 digits\",\"severity\":\"error\"}"
    "]"
    "}";

static const char *SCHEMA_UNIQUE =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\","
    "\"validate\":["
        "{\"type\":\"unique\",\"fields\":[\"id\"],\"severity\":\"error\"}"
    "]"
    "}";

static const char *SCHEMA_OUTLIER =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\","
    "\"validate\":["
        "{\"type\":\"outlier\",\"field\":\"lat\",\"method\":\"iqr\",\"factor\":1.5,\"severity\":\"warning\"}"
    "]"
    "}";

/* ============================================================================
 * Tests
 * ============================================================================ */

TEST(validate_null_input)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    NxValidateStatus s = nx_validate(NULL, 0, SCHEMA_NO_VALIDATION, strlen(SCHEMA_NO_VALIDATION),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_ERR_NULL);

    s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON), NULL, 0,
                   arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_ERR_NULL);

    sh_arena_free(arena);
}

TEST(validate_no_rules_passthrough)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    NxValidateStatus s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON),
                                     SCHEMA_NO_VALIDATION, strlen(SCHEMA_NO_VALIDATION),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);
    ASSERT(out_len > 0);

    /* Should be unchanged */
    ASSERT_EQ(out_len, strlen(CANONICAL_JSON));

    free(out_json);
    sh_arena_free(arena);
}

TEST(validate_geo_bounds_pass)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    /* All records within bounds [45,49] x [16,23] */
    NxValidateStatus s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON),
                                     SCHEMA_GEO_BOUNDS, strlen(SCHEMA_GEO_BOUNDS),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    /* Parse output and check validation section */
    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");
    ASSERT(validation != NULL);

    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "rules_applied"), 0), 1);
    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "errors"), -1), 0);

    /* All 3 records should be accepted (but 1 duplicate will be removed if unique is also tested) */
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0), 3);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(validate_format_pass)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    /* All zip codes are 4 digits */
    NxValidateStatus s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON),
                                     SCHEMA_FORMAT, strlen(SCHEMA_FORMAT),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");
    ASSERT(validation != NULL);

    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "errors"), -1), 0);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(validate_unique_removes_duplicates)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    /* Record 0 and 2 have same id "loc-1" - duplicate should be removed */
    NxValidateStatus s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON),
                                     SCHEMA_UNIQUE, strlen(SCHEMA_UNIQUE),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");
    ASSERT(validation != NULL);

    /* Should have 1 error (duplicate) */
    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "errors"), -1), 1);

    /* 2 records should remain (first occurrence kept, duplicate removed) */
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), 0), 1);

    /* Check details */
    ShJsonValue *details = sh_json_get(validation, "details");
    ASSERT(sh_json_type(details) == SH_JSON_ARRAY);
    ASSERT_EQ(sh_json_array_len(details), 1);

    ShJsonValue *detail0 = sh_json_array_get(details, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(detail0, "rule"), ""), "unique");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(detail0, "severity"), ""), "error");
    ASSERT_EQ(sh_json_as_int(sh_json_get(detail0, "row"), -1), 2);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(validate_outlier_warning)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    /* With only 3 values (47.5, 48.0, 47.5), unlikely to have outliers */
    /* This test mainly checks that outlier rule doesn't crash */
    NxValidateStatus s = nx_validate(CANONICAL_JSON, strlen(CANONICAL_JSON),
                                     SCHEMA_OUTLIER, strlen(SCHEMA_OUTLIER),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");
    ASSERT(validation != NULL);

    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "rules_applied"), 0), 1);

    /* Warning severity doesn't remove records */
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0), 3);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(validate_status_strings)
{
    ASSERT(strlen(nx_validate_status_str(NX_VALIDATE_OK)) > 0);
    ASSERT(strlen(nx_validate_status_str(NX_VALIDATE_ERR_NULL)) > 0);
    ASSERT(strlen(nx_validate_status_str(NX_VALIDATE_ERR_JSON)) > 0);
    ASSERT(strlen(nx_validate_status_str(NX_VALIDATE_ERR_ARENA)) > 0);
}

TEST(validate_geo_bounds_out_of_bounds)
{
    /* Create canonical JSON with out-of-bounds coordinates */
    const char *bad_json =
        "{"
        "\"nx_canonical\":1,"
        "\"schema_version\":\"test-v1\","
        "\"source_sha256\":\"abc123\","
        "\"output_type\":\"facility\","
        "\"records\":["
            "{\"id\":\"loc-1\",\"lat\":44.0,\"lon\":19.0}," /* Lat out of bounds [45,49] */
            "{\"id\":\"loc-2\",\"lat\":48.0,\"lon\":20.0}"
        "],"
        "\"record_count\":2,"
        "\"audit\":{"
            "\"rows_processed\":2,"
            "\"rows_accepted\":2,"
            "\"rows_rejected\":0,"
            "\"rejections\":[]"
        "}"
        "}";

    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    NxValidateStatus s = nx_validate(bad_json, strlen(bad_json),
                                     SCHEMA_GEO_BOUNDS, strlen(SCHEMA_GEO_BOUNDS),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");

    /* Should have 1 error */
    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "errors"), -1), 1);

    /* 1 record removed */
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0), 1);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), 0), 1);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(validate_format_invalid)
{
    /* Create canonical JSON with invalid zip code */
    const char *bad_json =
        "{"
        "\"nx_canonical\":1,"
        "\"schema_version\":\"test-v1\","
        "\"source_sha256\":\"abc123\","
        "\"output_type\":\"facility\","
        "\"records\":["
            "{\"id\":\"loc-1\",\"zip\":\"12345\"}," /* 5 digits, should be 4 */
            "{\"id\":\"loc-2\",\"zip\":\"5678\"}"
        "],"
        "\"record_count\":2,"
        "\"audit\":{"
            "\"rows_processed\":2,"
            "\"rows_accepted\":2,"
            "\"rows_rejected\":0,"
            "\"rejections\":[]"
        "}"
        "}";

    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out_json = NULL;
    size_t out_len = 0;

    NxValidateStatus s = nx_validate(bad_json, strlen(bad_json),
                                     SCHEMA_FORMAT, strlen(SCHEMA_FORMAT),
                                     arena, NULL, &out_json, &out_len);
    ASSERT_EQ(s, NX_VALIDATE_OK);
    ASSERT(out_json != NULL);

    SHArena *pa = sh_arena_create(1024 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out_json, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ShJsonValue *validation = sh_json_get(audit, "validation");

    /* Should have 1 error */
    ASSERT_EQ(sh_json_as_int(sh_json_get(validation, "errors"), -1), 1);

    /* 1 record removed */
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0), 1);

    free(out_json);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nValidation Engine Tests:\n");

    RUN_TEST(validate_null_input);
    RUN_TEST(validate_no_rules_passthrough);
    RUN_TEST(validate_geo_bounds_pass);
    RUN_TEST(validate_format_pass);
    RUN_TEST(validate_unique_removes_duplicates);
    RUN_TEST(validate_outlier_warning);
    RUN_TEST(validate_status_strings);
    RUN_TEST(validate_geo_bounds_out_of_bounds);
    RUN_TEST(validate_format_invalid);

    printf("\nValidation: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
