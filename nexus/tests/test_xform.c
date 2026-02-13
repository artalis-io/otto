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
 * Multi-Transform Tests (Phase 2A)
 * ============================================================================ */

/* Helper: apply schema to raw data and return parsed output root */
static ShJsonValue *apply_and_parse(const char *raw, const char *schema,
                                     SHArena **out_arena, char **out_json)
{
    size_t out_len = 0;
    SHArena *work = sh_arena_create(256 * 1024);
    NxXformStatus s = nx_xform_apply(raw, strlen(raw), schema, strlen(schema),
                                      work, out_json, &out_len);
    sh_arena_free(work);
    if (s != NX_XFORM_OK || !*out_json) return NULL;

    *out_arena = sh_arena_create(256 * 1024);
    ShJsonValue *root = NULL;
    sh_json_parse(*out_json, out_len, *out_arena, &root);
    return root;
}

/* Raw data: 3 columns, 2 rows */
static const char *RAW_MULTI =
    "{"
    "\"nx_raw\":1,"
    "\"source\":{\"filename\":\"test\",\"sha256\":\"abc\",\"format\":\"csv\"},"
    "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"A\",\"B\",\"C\"],"
    "\"header_row\":0,"
    "\"rows\":["
        "{\"row\":1,\"cells\":[\"hello,world\",\"Budapest\",\"47.4979 19.0402\"]},"
        "{\"row\":2,\"cells\":[\"foo,bar,baz\",\"Debrecen\",\"47.5316 21.6273\"]}"
    "],\"row_count\":2,\"col_count\":3}],\"warnings\":[]}";

TEST(multi_split_delimiter)
{
    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-split\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"delimiter\":\",\","
            "\"targets\":["
                "{\"field\":\"first\",\"index\":0,\"type\":\"string\"},"
                "{\"field\":\"second\",\"index\":1,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":3,\"target\":\"first\",\"type\":\"string\"},"
            "{\"source\":4,\"target\":\"second\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_MULTI, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 2);

    /* Row 1: "hello,world" → first="hello", second="world" */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "first"), ""), "hello");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "second"), ""), "world");

    /* Row 2: "foo,bar,baz" → first="foo", second="bar" */
    ShJsonValue *r1 = sh_json_array_get(records, 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "first"), ""), "foo");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "second"), ""), "bar");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_split_fixed_width)
{
    /* Raw with fixed-width data in col 2 */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"data\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"ABCD1234WXYZ\"]}],"
        "\"row_count\":1,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-fw\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"mode\":\"fixed_width\","
            "\"widths\":[4,4,4],"
            "\"targets\":["
                "{\"field\":\"p1\",\"index\":0,\"type\":\"string\"},"
                "{\"field\":\"p2\",\"index\":1,\"type\":\"string\"},"
                "{\"field\":\"p3\",\"index\":2,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"p1\",\"type\":\"string\"},"
            "{\"source\":2,\"target\":\"p2\",\"type\":\"string\"},"
            "{\"source\":3,\"target\":\"p3\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "p1"), ""), "ABCD");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "p2"), ""), "1234");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "p3"), ""), "WXYZ");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_split_with_trim)
{
    /* Split with per-target trim transforms */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"data\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\" hello ; world \"]}],"
        "\"row_count\":1,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-split-trim\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"delimiter\":\";\","
            "\"targets\":["
                "{\"field\":\"a\",\"index\":0,\"type\":\"string\",\"transforms\":[\"trim\"]},"
                "{\"field\":\"b\",\"index\":1,\"type\":\"string\",\"transforms\":[\"trim\"]}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"a\",\"type\":\"string\"},"
            "{\"source\":2,\"target\":\"b\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "a"), ""), "hello");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "b"), ""), "world");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_merge_separator)
{
    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-merge\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"merge\",\"sources\":[1,0],\"separator\":\", \","
            "\"target\":\"full\",\"target_type\":\"string\"}"
        "],"
        "\"columns\":["
            "{\"source\":3,\"target\":\"full\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_MULTI, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    /* sources [1, 0] = ["Budapest", "hello,world"] joined with ", " */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "full"), ""),
                 "Budapest, hello,world");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_merge_template)
{
    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-merge-tpl\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"merge\",\"sources\":[1,0],"
            "\"template\":\"{0} ({1})\","
            "\"target\":\"label\",\"target_type\":\"string\"}"
        "],"
        "\"columns\":["
            "{\"source\":3,\"target\":\"label\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_MULTI, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    /* {0}=Budapest, {1}=hello,world → "Budapest (hello,world)" */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "label"), ""),
                 "Budapest (hello,world)");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_regex_extract)
{
    /* col 2 has "47.4979 19.0402" — extract lat/lon with regex */
    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-regex\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"regex\",\"source\":2,"
            "\"pattern\":\"^([0-9.]+) ([0-9.]+)$\","
            "\"targets\":["
                "{\"field\":\"lat\",\"group\":1,\"type\":\"double\"},"
                "{\"field\":\"lon\",\"group\":2,\"type\":\"double\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":3,\"target\":\"lat\",\"type\":\"double\",\"precision\":6},"
            "{\"source\":4,\"target\":\"lon\",\"type\":\"double\",\"precision\":6}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_MULTI, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lat"), 0), 47.4979, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lon"), 0), 19.0402, 0.001);

    ShJsonValue *r1 = sh_json_array_get(sh_json_get(root, "records"), 1);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r1, "lat"), 0), 47.5316, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r1, "lon"), 0), 21.6273, 0.001);

    free(out);
    sh_arena_free(pa);
}

TEST(multi_regex_zip_city)
{
    /* Parse "1052 Budapest" → zip=1052, city=Budapest */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"addr\"],"
        "\"header_row\":0,"
        "\"rows\":["
            "{\"row\":1,\"cells\":[\"1052 Budapest\"]},"
            "{\"row\":2,\"cells\":[\"4032 Debrecen\"]}"
        "],\"row_count\":2,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-zip-city\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"regex\",\"source\":0,"
            "\"pattern\":\"^([0-9]{4}) (.+)$\","
            "\"targets\":["
                "{\"field\":\"zip\",\"group\":1,\"type\":\"string\"},"
                "{\"field\":\"city\",\"group\":2,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"zip\",\"type\":\"string\"},"
            "{\"source\":2,\"target\":\"city\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "zip"), ""), "1052");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "Budapest");

    ShJsonValue *r1 = sh_json_array_get(sh_json_get(root, "records"), 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "zip"), ""), "4032");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "city"), ""), "Debrecen");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_compute_coalesce)
{
    /* Test coalesce: first non-empty from columns 0, 1, 2 */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"a\",\"b\",\"c\"],"
        "\"header_row\":0,"
        "\"rows\":["
            "{\"row\":1,\"cells\":[\"\",\"\",\"fallback\"]},"
            "{\"row\":2,\"cells\":[\"primary\",\"secondary\",\"tertiary\"]}"
        "],\"row_count\":2,\"col_count\":3}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-coalesce\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"compute\",\"function\":\"coalesce\","
            "\"sources\":[0,1,2],"
            "\"targets\":[{\"field\":\"val\",\"type\":\"string\"}]}"
        "],"
        "\"columns\":["
            "{\"source\":3,\"target\":\"val\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 2);

    /* Row 1: "", "", "fallback" → coalesce = "fallback" */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "val"), ""), "fallback");

    /* Row 2: "primary", ... → coalesce = "primary" */
    ShJsonValue *r1 = sh_json_array_get(records, 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "val"), ""), "primary");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_compute_phone_normalize)
{
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"phone\"],"
        "\"header_row\":0,"
        "\"rows\":["
            "{\"row\":1,\"cells\":[\"06-30-123-4567\"]},"
            "{\"row\":2,\"cells\":[\"+36 20 987 6543\"]}"
        "],\"row_count\":2,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-phone\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"compute\",\"function\":\"phone_normalize\","
            "\"sources\":[0],"
            "\"targets\":[{\"field\":\"phone\",\"type\":\"string\"}]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"phone\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "phone"), ""), "+36301234567");

    ShJsonValue *r1 = sh_json_array_get(sh_json_get(root, "records"), 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "phone"), ""), "+36209876543");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_conditional_basic)
{
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"code\"],"
        "\"header_row\":0,"
        "\"rows\":["
            "{\"row\":1,\"cells\":[\"HU\"]},"
            "{\"row\":2,\"cells\":[\"Budapest\"]},"
            "{\"row\":3,\"cells\":[\"DE\"]}"
        "],\"row_count\":3,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-cond\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"conditional\",\"source\":0,"
            "\"conditions\":["
                "{\"match\":\"^[A-Z]{2}$\",\"set\":{\"field\":\"country\",\"value\":\"{0}\"}},"
                "{\"match\":\".*\",\"set\":{\"field\":\"country\",\"value\":\"HU\"}}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"country\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 3);

    /* "HU" matches ^[A-Z]{2}$ → country = "HU" (via {0}) */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "country"), ""), "HU");

    /* "Budapest" doesn't match ^[A-Z]{2}$, matches .* → country = "HU" */
    ShJsonValue *r1 = sh_json_array_get(records, 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r1, "country"), ""), "HU");

    /* "DE" matches ^[A-Z]{2}$ → country = "DE" */
    ShJsonValue *r2 = sh_json_array_get(records, 2);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r2, "country"), ""), "DE");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_chained_transforms)
{
    /* Split col 0, then merge the pieces with col 1 */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"coords\",\"name\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"47.50,19.04\",\"Budapest\"]}],"
        "\"row_count\":1,\"col_count\":2}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-chain\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"delimiter\":\",\","
            "\"targets\":["
                "{\"field\":\"lat\",\"index\":0,\"type\":\"string\"},"
                "{\"field\":\"lon\",\"index\":1,\"type\":\"string\"}"
            "]},"
            "{\"type\":\"merge\",\"sources\":[1,2,3],"
            "\"template\":\"{0} at ({1},{2})\","
            "\"target\":\"label\",\"target_type\":\"string\"}"
        "],"
        "\"columns\":["
            "{\"source\":2,\"target\":\"lat\",\"type\":\"double\",\"precision\":6},"
            "{\"source\":3,\"target\":\"lon\",\"type\":\"double\",\"precision\":6},"
            "{\"source\":4,\"target\":\"label\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lat"), 0), 47.50, 0.01);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lon"), 0), 19.04, 0.01);
    /* merge: {0}=name(col1), {1}=lat(virtual2), {2}=lon(virtual3) */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "label"), ""),
                 "Budapest at (47.50,19.04)");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_unknown_compute_function)
{
    /* Unknown compute function should produce empty virtual columns */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"a\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"test\"]}],"
        "\"row_count\":1,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-unknown\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"compute\",\"function\":\"nonexistent\","
            "\"sources\":[0],"
            "\"targets\":[{\"field\":\"out\",\"type\":\"string\"}]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"out\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    /* Unknown function → empty output */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "out"), ""), "");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_v1_schema_backward_compat)
{
    /* v1 schema (no multi_transforms) still works fine */
    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_JSON, SCHEMA_BASIC, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *records = sh_json_get(root, "records");
    ASSERT_EQ(sh_json_array_len(records), 2);

    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "Budapest");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_virtual_col_with_original)
{
    /* Mix original columns and virtual columns in output */
    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-mix\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"delimiter\":\",\","
            "\"targets\":["
                "{\"field\":\"a\",\"index\":0,\"type\":\"string\"},"
                "{\"field\":\"b\",\"index\":1,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"city\",\"type\":\"string\"},"
            "{\"source\":3,\"target\":\"first_part\",\"type\":\"string\"},"
            "{\"source\":4,\"target\":\"second_part\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(RAW_MULTI, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    /* col 1 = "Budapest" (original), virtual 3 = "hello", virtual 4 = "world" */
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "first_part"), ""), "hello");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "second_part"), ""), "world");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_regex_no_match)
{
    /* When regex doesn't match, virtual columns should be empty */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"data\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"no-match-here\"]}],"
        "\"row_count\":1,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-nomatch\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"regex\",\"source\":0,"
            "\"pattern\":\"^([0-9]{4}) (.+)$\","
            "\"targets\":["
                "{\"field\":\"zip\",\"group\":1,\"type\":\"string\"},"
                "{\"field\":\"city\",\"group\":2,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"zip\",\"type\":\"string\"},"
            "{\"source\":2,\"target\":\"city\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "zip"), ""), "");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "city"), ""), "");

    free(out);
    sh_arena_free(pa);
}

TEST(multi_split_three_pieces)
{
    /* Split with 3 pieces, select specific indices */
    const char *raw =
        "{"
        "\"nx_raw\":1,"
        "\"source\":{\"filename\":\"\",\"sha256\":\"\",\"format\":\"csv\"},"
        "\"tables\":[{\"name\":\"S\",\"index\":0,\"headers\":[\"data\"],"
        "\"header_row\":0,"
        "\"rows\":[{\"row\":1,\"cells\":[\"alpha|beta|gamma|delta\"]}],"
        "\"row_count\":1,\"col_count\":1}],\"warnings\":[]}";

    const char *schema =
        "{"
        "\"nx_schema\":2,"
        "\"version\":\"test-split3\","
        "\"output_type\":\"test\","
        "\"multi_transforms\":["
            "{\"type\":\"split\",\"source\":0,\"delimiter\":\"|\","
            "\"targets\":["
                "{\"field\":\"first\",\"index\":0,\"type\":\"string\"},"
                "{\"field\":\"third\",\"index\":2,\"type\":\"string\"}"
            "]}"
        "],"
        "\"columns\":["
            "{\"source\":1,\"target\":\"first\",\"type\":\"string\"},"
            "{\"source\":2,\"target\":\"third\",\"type\":\"string\"}"
        "]"
        "}";

    char *out = NULL;
    SHArena *pa = NULL;
    ShJsonValue *root = apply_and_parse(raw, schema, &pa, &out);
    ASSERT(root != NULL);

    ShJsonValue *r0 = sh_json_array_get(sh_json_get(root, "records"), 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "first"), ""), "alpha");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(r0, "third"), ""), "gamma");

    free(out);
    sh_arena_free(pa);
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

    /* Multi-transform tests (Phase 2A) */
    RUN_TEST(multi_split_delimiter);
    RUN_TEST(multi_split_fixed_width);
    RUN_TEST(multi_split_with_trim);
    RUN_TEST(multi_split_three_pieces);
    RUN_TEST(multi_merge_separator);
    RUN_TEST(multi_merge_template);
    RUN_TEST(multi_regex_extract);
    RUN_TEST(multi_regex_zip_city);
    RUN_TEST(multi_regex_no_match);
    RUN_TEST(multi_compute_coalesce);
    RUN_TEST(multi_compute_phone_normalize);
    RUN_TEST(multi_conditional_basic);
    RUN_TEST(multi_chained_transforms);
    RUN_TEST(multi_unknown_compute_function);
    RUN_TEST(multi_v1_schema_backward_compat);
    RUN_TEST(multi_virtual_col_with_original);

    /* Pipeline tests */
    RUN_TEST(pipeline_xlsx_to_canonical);
    RUN_TEST(pipeline_deterministic);
    RUN_TEST(pipeline_invalid_format);
    RUN_TEST(ingest_status_strings);

    printf("\nTransform + Pipeline: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
