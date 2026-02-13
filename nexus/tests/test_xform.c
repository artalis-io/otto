/*
 * test_xform.c - Tests for transform engine and full pipeline
 */

#include "nx_xform.h"
#include "nx_ingest.h"
#include "nx_slug.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Embedded XLSX fixture */
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
 * Test Schemas
 * ============================================================================ */

static const char *SCHEMA_BASIC =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-v1\","
    "\"output_type\":\"facility\","
    "\"table_selector\":{\"index\":0},"
    "\"skip_rows\":0,"
    "\"columns\":["
        "{\"source\":0,\"target\":\"city\",\"type\":\"string\",\"transforms\":[\"trim\"],\"required\":true},"
        "{\"source\":1,\"target\":\"name\",\"type\":\"string\",\"transforms\":[\"trim\"],\"required\":true},"
        "{\"source\":2,\"target\":\"address\",\"type\":\"string\",\"transforms\":[\"trim\"],\"default\":\"\"},"
        "{\"source\":3,\"target\":\"lat\",\"type\":\"double\",\"precision\":4,\"required\":true,"
            "\"validate\":{\"min\":-90.0,\"max\":90.0}},"
        "{\"source\":4,\"target\":\"lon\",\"type\":\"double\",\"precision\":4,\"required\":true,"
            "\"validate\":{\"min\":-180.0,\"max\":180.0}}"
    "],"
    "\"derived\":["
        "{\"target\":\"facility_type\",\"value\":\"depot\"},"
        "{\"target\":\"operator\",\"value\":\"GLS\"},"
        "{\"target\":\"country\",\"value\":\"HU\"}"
    "],"
    "\"row_id\":{\"template\":\"gls-hu-{city}-{name}\",\"slugify\":true}"
    "}";

/* Schema that makes lat required but source col is empty → rejection */
static const char *SCHEMA_REQUIRED_REJECT =
    "{"
    "\"nx_schema\":1,"
    "\"version\":\"test-reject\","
    "\"output_type\":\"test\","
    "\"table_selector\":{\"index\":0},"
    "\"columns\":["
        "{\"source\":0,\"target\":\"name\",\"type\":\"string\",\"required\":true},"
        "{\"source\":99,\"target\":\"missing\",\"type\":\"string\",\"required\":true}"
    "]"
    "}";

/* Raw rows JSON matching our fixture output structure */
static const char *RAW_JSON =
    "{"
    "\"nx_raw\":1,"
    "\"source\":{\"filename\":\"test.xlsx\",\"sha256\":\"abc123\",\"format\":\"xlsx\"},"
    "\"tables\":[{"
        "\"name\":\"Sheet1\","
        "\"index\":0,"
        "\"headers\":[\"City\",\"Name\",\"Address\",\"Lat\",\"Lon\"],"
        "\"header_row\":0,"
        "\"rows\":["
            "{\"row\":1,\"cells\":[\"Budapest\",\"Depot #1\",\"Futó u. 35-37\",\"47.4799\",\"19.07\"]},"
            "{\"row\":2,\"cells\":[\"Debrecen\",\"Depot #2\",\"Balmazújvárosi út\",\"47.5316\",\"21.6273\"]}"
        "],"
        "\"row_count\":2,"
        "\"col_count\":5"
    "}],"
    "\"warnings\":[]"
    "}";

/* ============================================================================
 * Slug Tests
 * ============================================================================ */

TEST(slug_basic)
{
    char buf[256];
    size_t len = nx_slugify("Hello World", 11, buf, sizeof(buf));
    ASSERT_STREQ(buf, "hello-world");
    ASSERT_EQ(len, 11);
}

TEST(slug_special_chars)
{
    char buf[256];
    nx_slugify("Depot #1 - Budapest/HU", 22, buf, sizeof(buf));
    ASSERT_STREQ(buf, "depot-1-budapest-hu");
}

TEST(slug_consecutive_hyphens)
{
    char buf[256];
    nx_slugify("a---b___c...d", 13, buf, sizeof(buf));
    ASSERT_STREQ(buf, "a-b-c-d");
}

TEST(slug_leading_trailing)
{
    char buf[256];
    nx_slugify("  hello  ", 9, buf, sizeof(buf));
    ASSERT_STREQ(buf, "hello");
}

TEST(slug_empty)
{
    char buf[256];
    size_t len = nx_slugify("", 0, buf, sizeof(buf));
    ASSERT_EQ(len, 0);
    ASSERT_STREQ(buf, "");
}

/* ============================================================================
 * Transform Tests
 * ============================================================================ */

TEST(xform_null_input)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(64 * 1024);
    NxXformStatus s = nx_xform_apply(NULL, 0, NULL, 0, arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_ERR_NULL);
    sh_arena_free(arena);
}

TEST(xform_basic_transform)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXformStatus s = nx_xform_apply(RAW_JSON, strlen(RAW_JSON),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_OK);
    ASSERT(out != NULL);

    /* Parse output */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out, out_len, pa, &root);

    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_canonical"), -1), 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(root, "schema_version"), ""), "test-v1");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(root, "output_type"), ""), "facility");

    /* Records */
    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 2);

    /* First record */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "name"), ""), "Depot #1");

    /* Derived fields */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "facility_type"), ""), "depot");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "operator"), ""), "GLS");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "country"), ""), "HU");

    /* Audit */
    ShJsonValue *audit = sh_json_get(root, "audit");
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_processed"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), -1), 0);

    free(out);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xform_type_coercion)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXformStatus s = nx_xform_apply(RAW_JSON, strlen(RAW_JSON),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out, out_len, pa, &root);

    /* lat and lon should be numbers, not strings */
    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_EQ(sh_json_type(sh_json_get(r0, "lat")), SH_JSON_NUMBER);
    ASSERT_EQ(sh_json_type(sh_json_get(r0, "lon")), SH_JSON_NUMBER);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lat"), 0), 47.4799, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lon"), 0), 19.07, 0.001);

    free(out);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xform_row_id_slugify)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXformStatus s = nx_xform_apply(RAW_JSON, strlen(RAW_JSON),
                                      SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                      arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out, out_len, pa, &root);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    const char *id = sh_json_as_string(sh_json_get(r0, "id"), "");
    ASSERT_STREQ(id, "gls-hu-budapest-depot-1");

    ShJsonValue *r1 = sh_json_array_get(sh_json_get(root, "records"), 1);
    const char *id1 = sh_json_as_string(sh_json_get(r1, "id"), "");
    ASSERT_STREQ(id1, "gls-hu-debrecen-depot-2");

    free(out);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xform_required_rejection)
{
    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(256 * 1024);

    NxXformStatus s = nx_xform_apply(RAW_JSON, strlen(RAW_JSON),
                                      SCHEMA_REQUIRED_REJECT,
                                      strlen(SCHEMA_REQUIRED_REJECT),
                                      arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_OK);

    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out, out_len, pa, &root);

    /* All rows should be rejected (col 99 doesn't exist, is required) */
    ShJsonValue *audit = sh_json_get(root, "audit");
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_processed"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), -1), 0);

    /* Check rejection details */
    ShJsonValue *rejections = sh_json_get(audit, "rejections");
    ASSERT_EQ(sh_json_array_len(rejections), 2);

    ShJsonValue *rej0 = sh_json_array_get(rejections, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(rej0, "field"), ""), "missing");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(rej0, "reason"), ""), "required field empty");

    free(out);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

TEST(xform_deterministic)
{
    SHArena *a1 = sh_arena_create(256 * 1024);
    SHArena *a2 = sh_arena_create(256 * 1024);
    char *out1 = NULL, *out2 = NULL;
    size_t len1 = 0, len2 = 0;

    nx_xform_apply(RAW_JSON, strlen(RAW_JSON), SCHEMA_BASIC,
                   strlen(SCHEMA_BASIC), a1, &out1, &len1);
    nx_xform_apply(RAW_JSON, strlen(RAW_JSON), SCHEMA_BASIC,
                   strlen(SCHEMA_BASIC), a2, &out2, &len2);

    ASSERT_EQ(len1, len2);
    ASSERT(memcmp(out1, out2, len1) == 0);

    free(out1);
    free(out2);
    sh_arena_free(a1);
    sh_arena_free(a2);
}

TEST(xform_validation_range)
{
    /* Create schema with strict lat range */
    const char *schema =
        "{"
        "\"nx_schema\":1,"
        "\"version\":\"range-test\","
        "\"output_type\":\"test\","
        "\"table_selector\":{\"index\":0},"
        "\"columns\":["
            "{\"source\":0,\"target\":\"val\",\"type\":\"double\","
            "\"required\":true,\"validate\":{\"min\":0,\"max\":10}}"
        "]"
        "}";

    /* Raw data with value=47 (out of range) */
    const char *raw =
        "{\"nx_raw\":1,\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"xlsx\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"V\"],\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"47\"]}],\"row_count\":1,\"col_count\":1}],"
        "\"warnings\":[]}";

    char *out = NULL;
    size_t out_len = 0;
    SHArena *arena = sh_arena_create(128 * 1024);

    NxXformStatus s = nx_xform_apply(raw, strlen(raw), schema, strlen(schema),
                                      arena, &out, &out_len);
    ASSERT_EQ(s, NX_XFORM_OK);

    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(out, out_len, pa, &root);

    ShJsonValue *audit = sh_json_get(root, "audit");
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), -1), 1);

    ShJsonValue *rej = sh_json_array_get(sh_json_get(audit, "rejections"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(rej, "reason"), ""), "above maximum");

    free(out);
    sh_arena_free(pa);
    sh_arena_free(arena);
}

/* ============================================================================
 * Full Pipeline Tests
 * ============================================================================ */

TEST(pipeline_xlsx_to_canonical)
{
    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;

    NxIngestStatus s = nx_ingest(MINIMAL_XLSX, MINIMAL_XLSX_LEN,
                                  NX_FORMAT_XLSX, "minimal.xlsx",
                                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                  &raw, &raw_len,
                                  &canon, &canon_len);
    ASSERT_EQ(s, NX_INGEST_OK);
    ASSERT(raw != NULL);
    ASSERT(canon != NULL);

    /* Verify canonical output */
    SHArena *pa = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(canon, canon_len, pa, &root);

    ASSERT_EQ(sh_json_as_int(sh_json_get(root, "nx_canonical"), -1), 1);

    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 2);

    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "id"), ""), "gls-hu-budapest-depot-1");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "Budapest");

    /* Audit shows all accepted */
    ShJsonValue *audit = sh_json_get(root, "audit");
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_accepted"), -1), 2);
    ASSERT_EQ(sh_json_as_int(sh_json_get(audit, "rows_rejected"), -1), 0);

    free(raw);
    free(canon);
    sh_arena_free(pa);
}

TEST(pipeline_deterministic)
{
    char *canon1 = NULL, *canon2 = NULL;
    size_t len1 = 0, len2 = 0;

    nx_ingest(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NX_FORMAT_XLSX, "test.xlsx",
              SCHEMA_BASIC, strlen(SCHEMA_BASIC), NULL, NULL, &canon1, &len1);
    nx_ingest(MINIMAL_XLSX, MINIMAL_XLSX_LEN, NX_FORMAT_XLSX, "test.xlsx",
              SCHEMA_BASIC, strlen(SCHEMA_BASIC), NULL, NULL, &canon2, &len2);

    ASSERT_EQ(len1, len2);
    ASSERT(memcmp(canon1, canon2, len1) == 0);

    free(canon1);
    free(canon2);
}

TEST(pipeline_invalid_format)
{
    char *canon = NULL;
    size_t canon_len = 0;
    NxIngestStatus s = nx_ingest("data", 4, (NxIngestFormat)99, "test",
                                  SCHEMA_BASIC, strlen(SCHEMA_BASIC),
                                  NULL, NULL, &canon, &canon_len);
    ASSERT_EQ(s, NX_INGEST_ERR_FORMAT);
}

TEST(xform_status_strings)
{
    ASSERT(strlen(nx_xform_status_str(NX_XFORM_OK)) > 0);
    ASSERT(strlen(nx_xform_status_str(NX_XFORM_ERR_NULL)) > 0);
    ASSERT(strlen(nx_xform_status_str(NX_XFORM_ERR_SCHEMA)) > 0);
    ASSERT(strlen(nx_xform_status_str(NX_XFORM_ERR_RAW)) > 0);
}

TEST(ingest_status_strings)
{
    ASSERT(strlen(nx_ingest_status_str(NX_INGEST_OK)) > 0);
    ASSERT(strlen(nx_ingest_status_str(NX_INGEST_ERR_STAGE_A)) > 0);
    ASSERT(strlen(nx_ingest_status_str(NX_INGEST_ERR_STAGE_B)) > 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nTransform + Pipeline Tests:\n");

    /* Slug tests */
    RUN_TEST(slug_basic);
    RUN_TEST(slug_special_chars);
    RUN_TEST(slug_consecutive_hyphens);
    RUN_TEST(slug_leading_trailing);
    RUN_TEST(slug_empty);

    /* Transform tests */
    RUN_TEST(xform_null_input);
    RUN_TEST(xform_basic_transform);
    RUN_TEST(xform_type_coercion);
    RUN_TEST(xform_row_id_slugify);
    RUN_TEST(xform_required_rejection);
    RUN_TEST(xform_deterministic);
    RUN_TEST(xform_validation_range);
    RUN_TEST(xform_status_strings);

    /* Pipeline tests */
    RUN_TEST(pipeline_xlsx_to_canonical);
    RUN_TEST(pipeline_deterministic);
    RUN_TEST(pipeline_invalid_format);
    RUN_TEST(ingest_status_strings);

    printf("\nTransform + Pipeline: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
