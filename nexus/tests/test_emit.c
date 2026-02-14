/*
 * test_emit.c - Tests for Nexus output emitters (GeoJSON + CSV)
 */

#include "nx_emit.h"
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
 * Test Fixtures
 * ============================================================================ */

static const char *CANONICAL_2_RECORDS =
    "{"
    "\"nx_canonical\":1,"
    "\"schema_version\":\"test-v1\","
    "\"output_type\":\"facility\","
    "\"records\":["
        "{\"id\":\"gls-hu-budapest\",\"city\":\"Budapest\",\"lat\":47.48,\"lon\":19.07,\"zip\":\"1239\",\"facility_type\":\"depot\"},"
        "{\"id\":\"gls-hu-debrecen\",\"city\":\"Debrecen\",\"lat\":47.53,\"lon\":21.63,\"zip\":\"4002\",\"facility_type\":\"depot\"}"
    "],"
    "\"record_count\":2,"
    "\"audit\":{}"
    "}";

static const char *CANONICAL_EMPTY_RECORDS =
    "{"
    "\"nx_canonical\":1,"
    "\"records\":[],"
    "\"record_count\":0"
    "}";

static const char *CANONICAL_MIXED_GEO =
    "{"
    "\"nx_canonical\":1,"
    "\"records\":["
        "{\"id\":\"ok\",\"lat\":47.48,\"lon\":19.07,\"city\":\"Budapest\"},"
        "{\"id\":\"no-geo\",\"city\":\"Unknown\"},"
        "{\"id\":\"ok2\",\"lat\":47.53,\"lon\":21.63,\"city\":\"Debrecen\"}"
    "],"
    "\"record_count\":3"
    "}";

static const char *CANONICAL_NUMERIC =
    "{"
    "\"nx_canonical\":1,"
    "\"records\":["
        "{\"id\":\"r1\",\"count\":42,\"price\":9.99,\"active\":true}"
    "],"
    "\"record_count\":1"
    "}";

/* ============================================================================
 * GeoJSON Tests
 * ============================================================================ */

TEST(emit_geojson_null_input)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_geojson(NULL, 0, NULL, arena, &out, &len), NX_EMIT_ERR_NULL);
    ASSERT_EQ(nx_emit_geojson("x", 1, NULL, arena, NULL, &len), NX_EMIT_ERR_NULL);
    ASSERT_EQ(nx_emit_geojson("x", 1, NULL, NULL, &out, &len), NX_EMIT_ERR_NULL);

    sh_arena_free(arena);
}

TEST(emit_geojson_invalid_json)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    ASSERT_EQ(nx_emit_geojson("{bad", 4, NULL, arena, &out, &len), NX_EMIT_ERR_JSON);
    sh_arena_free(arena);
}

TEST(emit_geojson_basic)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    NxEmitGeoJsonOpts opts = NX_EMIT_GEOJSON_DEFAULTS;

    ASSERT_EQ(nx_emit_geojson(CANONICAL_2_RECORDS, strlen(CANONICAL_2_RECORDS),
              &opts, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    ASSERT(len > 0);

    /* Parse and verify structure */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT_EQ(sh_json_parse(out, len, pa, &root), SH_JSON_OK);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(root, "type"), ""), "FeatureCollection");

    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT_EQ(sh_json_array_len(features), 2);

    /* First feature */
    ShJsonValue *f0 = sh_json_array_get(features, 0);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(f0, "id"), ""), "gls-hu-budapest");

    /* Geometry */
    ShJsonValue *geom = sh_json_get(f0, "geometry");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(geom, "type"), ""), "Point");
    ShJsonValue *coords = sh_json_get(geom, "coordinates");
    ASSERT_EQ(sh_json_array_len(coords), 2);

    /* Properties should have city, zip, facility_type (not lat/lon/id) */
    ShJsonValue *props = sh_json_get(f0, "properties");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(props, "city"), ""), "Budapest");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(props, "zip"), ""), "1239");
    ASSERT(sh_json_get(props, "lat") == NULL);  /* lat excluded from properties */
    ASSERT(sh_json_get(props, "lon") == NULL);  /* lon excluded from properties */
    ASSERT(sh_json_get(props, "id") == NULL);   /* id excluded from properties */

    sh_arena_free(pa);
    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_coordinate_order)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_geojson(CANONICAL_2_RECORDS, strlen(CANONICAL_2_RECORDS),
              NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);

    /* RFC 7946: [lon, lat] — Budapest: lon=19.07, lat=47.48 */
    /* lon should come first in coordinates array */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT_EQ(sh_json_parse(out, len, pa, &root), SH_JSON_OK);

    ShJsonValue *f0 = sh_json_array_get(sh_json_get(root, "features"), 0);
    ShJsonValue *coords = sh_json_get(sh_json_get(f0, "geometry"), "coordinates");
    double c0 = sh_json_as_double(sh_json_array_get(coords, 0), 0);
    double c1 = sh_json_as_double(sh_json_array_get(coords, 1), 0);

    /* First coord should be lon (19.07), second should be lat (47.48) */
    ASSERT(fabs(c0 - 19.07) < 0.001);
    ASSERT(fabs(c1 - 47.48) < 0.001);

    sh_arena_free(pa);
    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_custom_fields)
{
    /* Test with custom lat/lon field names */
    const char *json =
        "{\"records\":[{\"id\":\"x\",\"latitude\":47.5,\"longitude\":19.0,\"name\":\"A\"}]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    NxEmitGeoJsonOpts opts = { "latitude", "longitude", "id", 6 };

    ASSERT_EQ(nx_emit_geojson(json, strlen(json), &opts, arena, &out, &len),
              NX_EMIT_OK);
    ASSERT(out != NULL);

    /* Verify a feature was produced */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT_EQ(sh_json_parse(out, len, pa, &root), SH_JSON_OK);
    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT_EQ(sh_json_array_len(features), 1);

    sh_arena_free(pa);
    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_missing_latlon)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_geojson(CANONICAL_MIXED_GEO, strlen(CANONICAL_MIXED_GEO),
              NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);

    /* Should have 2 features (the one without lat/lon skipped) */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT_EQ(sh_json_parse(out, len, pa, &root), SH_JSON_OK);
    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT_EQ(sh_json_array_len(features), 2);

    sh_arena_free(pa);
    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_no_records)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_geojson(CANONICAL_EMPTY_RECORDS, strlen(CANONICAL_EMPTY_RECORDS),
              NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);

    /* Should be empty FeatureCollection */
    SHArena *pa = sh_arena_create(64 * 1024);
    ShJsonValue *root = NULL;
    ASSERT_EQ(sh_json_parse(out, len, pa, &root), SH_JSON_OK);
    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT_EQ(sh_json_array_len(features), 0);

    sh_arena_free(pa);
    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_precision)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    NxEmitGeoJsonOpts opts = NX_EMIT_GEOJSON_DEFAULTS;
    opts.precision = 2;

    ASSERT_EQ(nx_emit_geojson(CANONICAL_2_RECORDS, strlen(CANONICAL_2_RECORDS),
              &opts, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    /* precision=2 should show 2 decimal places */
    ASSERT(strstr(out, "19.07") != NULL);
    ASSERT(strstr(out, "47.48") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_geojson_status_strings)
{
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_OK), "ok");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_NULL), "null input");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_JSON), "invalid JSON");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_NO_RECORDS), "no records array");
}

/* ============================================================================
 * CSV Tests
 * ============================================================================ */

TEST(emit_csv_null_input)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(NULL, 0, NULL, arena, &out, &len), NX_EMIT_ERR_NULL);
    ASSERT_EQ(nx_emit_csv("x", 1, NULL, arena, NULL, &len), NX_EMIT_ERR_NULL);
    ASSERT_EQ(nx_emit_csv("x", 1, NULL, NULL, &out, &len), NX_EMIT_ERR_NULL);

    sh_arena_free(arena);
}

TEST(emit_csv_invalid_json)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    ASSERT_EQ(nx_emit_csv("{bad", 4, NULL, arena, &out, &len), NX_EMIT_ERR_JSON);
    sh_arena_free(arena);
}

TEST(emit_csv_basic)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(CANONICAL_2_RECORDS, strlen(CANONICAL_2_RECORDS),
              NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    ASSERT(len > 0);

    /* Should have header + 2 data rows = 3 lines (each ending with \r\n) */
    int line_count = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (out[i] == '\r' && out[i + 1] == '\n') line_count++;
    }
    ASSERT_EQ(line_count, 3);

    /* Header should contain field names */
    ASSERT(strstr(out, "id") != NULL);
    ASSERT(strstr(out, "city") != NULL);
    ASSERT(strstr(out, "lat") != NULL);
    ASSERT(strstr(out, "lon") != NULL);

    /* Data should contain values */
    ASSERT(strstr(out, "Budapest") != NULL);
    ASSERT(strstr(out, "Debrecen") != NULL);
    ASSERT(strstr(out, "gls-hu-budapest") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_quoting)
{
    /* Field containing comma should be quoted */
    const char *json =
        "{\"records\":[{\"name\":\"Foo, Bar\",\"city\":\"Budapest\"}]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(json, strlen(json), NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "\"Foo, Bar\"") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_quote_escape)
{
    const char *json =
        "{\"records\":[{\"name\":\"He said \\\"hello\\\"\"}]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(json, strlen(json), NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    /* Should have double-quoted quotes: "He said ""hello""" */
    ASSERT(strstr(out, "\"He said \"\"hello\"\"\"") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_custom_delimiter)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;
    NxEmitCsvOpts opts = { ';' };

    ASSERT_EQ(nx_emit_csv(CANONICAL_2_RECORDS, strlen(CANONICAL_2_RECORDS),
              &opts, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    /* Fields separated by semicolons */
    ASSERT(strstr(out, ";") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_no_records)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(CANONICAL_EMPTY_RECORDS, strlen(CANONICAL_EMPTY_RECORDS),
              NULL, arena, &out, &len), NX_EMIT_OK);
    /* Empty records → NULL output (no header because no first record) */
    ASSERT(out == NULL);

    sh_arena_free(arena);
}

TEST(emit_csv_numeric_fields)
{
    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(CANONICAL_NUMERIC, strlen(CANONICAL_NUMERIC),
              NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);
    /* Should contain numeric values as strings */
    ASSERT(strstr(out, "42") != NULL);
    ASSERT(strstr(out, "9.99") != NULL);
    ASSERT(strstr(out, "true") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_all_field_types)
{
    const char *json =
        "{\"records\":["
        "{\"str\":\"hello\",\"num\":42,\"dbl\":3.14,\"flag\":true,\"empty\":null}"
        "]}";

    SHArena *arena = sh_arena_create(64 * 1024);
    char *out = NULL; size_t len = 0;

    ASSERT_EQ(nx_emit_csv(json, strlen(json), NULL, arena, &out, &len), NX_EMIT_OK);
    ASSERT(out != NULL);

    /* Header */
    ASSERT(strstr(out, "str") != NULL);
    ASSERT(strstr(out, "num") != NULL);
    /* Data */
    ASSERT(strstr(out, "hello") != NULL);
    ASSERT(strstr(out, "42") != NULL);
    ASSERT(strstr(out, "3.14") != NULL);
    ASSERT(strstr(out, "true") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(emit_csv_status_strings)
{
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_OK), "ok");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_NULL), "null input");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_JSON), "invalid JSON");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_NO_RECORDS), "no records array");
    ASSERT_STREQ(nx_emit_status_str(NX_EMIT_ERR_ALLOC), "allocation failed");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nNexus Emit Tests:\n");

    printf("\n  GeoJSON:\n");
    RUN_TEST(emit_geojson_null_input);
    RUN_TEST(emit_geojson_invalid_json);
    RUN_TEST(emit_geojson_basic);
    RUN_TEST(emit_geojson_coordinate_order);
    RUN_TEST(emit_geojson_custom_fields);
    RUN_TEST(emit_geojson_missing_latlon);
    RUN_TEST(emit_geojson_no_records);
    RUN_TEST(emit_geojson_precision);
    RUN_TEST(emit_geojson_status_strings);

    printf("\n  CSV:\n");
    RUN_TEST(emit_csv_null_input);
    RUN_TEST(emit_csv_invalid_json);
    RUN_TEST(emit_csv_basic);
    RUN_TEST(emit_csv_quoting);
    RUN_TEST(emit_csv_quote_escape);
    RUN_TEST(emit_csv_custom_delimiter);
    RUN_TEST(emit_csv_no_records);
    RUN_TEST(emit_csv_numeric_fields);
    RUN_TEST(emit_csv_all_field_types);
    RUN_TEST(emit_csv_status_strings);

    printf("\nNexus Emit: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
