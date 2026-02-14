/*
 * test_geojson.c - Unit tests for GeoJSON encoder
 */

#include "sh_geojson.h"
#include "sh_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Helper: create writer + buf, return GeoJSON string (caller frees) */
static char *geojson_to_string(void (*build_fn)(ShJsonWriter *w))
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    build_fn(&w);
    ASSERT(!sh_json_writer_error(&w));
    char *result = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    return result;
}

/* ============================================================================
 * Tests
 * ============================================================================ */

static void build_empty(ShJsonWriter *w) {
    sh_geojson_begin(w);
    sh_geojson_end(w);
}

TEST(geojson_empty_collection)
{
    char *out = geojson_to_string(build_empty);
    ASSERT(out != NULL);
    ASSERT_STREQ(out, "{\"type\":\"FeatureCollection\",\"features\":[]}");
    free(out);
}

static void build_single_point(ShJsonWriter *w) {
    sh_geojson_begin(w);
    ShGeoJsonProp props[] = { {"city", "Budapest"}, {"type", "depot"} };
    sh_geojson_point_feature(w, "loc-1", 19.07, 47.48, 6, props, 2);
    sh_geojson_end(w);
}

TEST(geojson_single_point)
{
    char *out = geojson_to_string(build_single_point);
    ASSERT(out != NULL);
    /* Verify it contains expected structure */
    ASSERT(strstr(out, "\"type\":\"FeatureCollection\"") != NULL);
    ASSERT(strstr(out, "\"type\":\"Feature\"") != NULL);
    ASSERT(strstr(out, "\"id\":\"loc-1\"") != NULL);
    ASSERT(strstr(out, "\"type\":\"Point\"") != NULL);
    ASSERT(strstr(out, "\"city\":\"Budapest\"") != NULL);
    ASSERT(strstr(out, "\"type\":\"depot\"") != NULL);
    free(out);
}

TEST(geojson_coordinate_order)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, 19.07, 47.48, 2, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    /* RFC 7946: coordinates are [lon, lat] */
    ASSERT(strstr(out, "[19.07,47.48]") != NULL);
    free(out);
}

TEST(geojson_precision)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, 19.07, 47.48, 2, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    /* precision=2 should give 2 decimal places */
    ASSERT(strstr(out, "19.07") != NULL);
    ASSERT(strstr(out, "47.48") != NULL);
    /* Should NOT have extra precision like 19.070000 */
    ASSERT(strstr(out, "19.070") == NULL);
    free(out);
}

TEST(geojson_no_id)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, 1.0, 2.0, 1, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    /* Should not have "id" field */
    ASSERT(strstr(out, "\"id\"") == NULL);
    free(out);
}

TEST(geojson_no_properties)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, "f1", 1.0, 2.0, 1, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    /* Should have empty properties object */
    ASSERT(strstr(out, "\"properties\":{}") != NULL);
    free(out);
}

TEST(geojson_multiple_features)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    ShGeoJsonProp p1[] = { {"name", "A"} };
    sh_geojson_point_feature(&w, "f1", 1.0, 2.0, 1, p1, 1);
    ShGeoJsonProp p2[] = { {"name", "B"} };
    sh_geojson_point_feature(&w, "f2", 3.0, 4.0, 1, p2, 1);
    ShGeoJsonProp p3[] = { {"name", "C"} };
    sh_geojson_point_feature(&w, "f3", 5.0, 6.0, 1, p3, 1);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);

    /* Parse and verify 3 features */
    SHArena *arena = sh_arena_create(4096);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(out, strlen(out), arena, &root) == SH_JSON_OK);
    ShJsonValue *features = sh_json_get(root, "features");
    ASSERT(features != NULL);
    ASSERT_EQ(sh_json_array_len(features), 3);
    sh_arena_free(arena);
    free(out);
}

TEST(geojson_special_chars_in_props)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    ShGeoJsonProp props[] = { {"name", "Foo \"Bar\" & <Baz>"} };
    sh_geojson_point_feature(&w, NULL, 0.0, 0.0, 1, props, 1);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);

    /* Verify valid JSON (quotes escaped) */
    SHArena *arena = sh_arena_create(4096);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(out, strlen(out), arena, &root) == SH_JSON_OK);

    ShJsonValue *feat = sh_json_array_get(sh_json_get(root, "features"), 0);
    ShJsonValue *name = sh_json_get(sh_json_get(feat, "properties"), "name");
    ASSERT_STREQ(sh_json_as_string(name, ""), "Foo \"Bar\" & <Baz>");

    sh_arena_free(arena);
    free(out);
}

TEST(geojson_negative_coordinates)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, -73.9857, -33.8688, 4, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "-73.9857") != NULL);
    ASSERT(strstr(out, "-33.8688") != NULL);
    free(out);
}

TEST(geojson_large_precision)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, 19.0712345678, 47.4823456789, 10, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "19.0712345678") != NULL);
    ASSERT(strstr(out, "47.4823456789") != NULL);
    free(out);
}

TEST(geojson_with_json_buf)
{
    /* Full roundtrip: writer → buf → take → parse → verify */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    ShGeoJsonProp props[] = { {"city", "Debrecen"}, {"zip", "4002"} };
    sh_geojson_point_feature(&w, "d1", 21.63, 47.53, 6, props, 2);
    sh_geojson_end(&w);

    ASSERT(!sh_json_writer_error(&w));
    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);

    /* Parse and verify structure */
    SHArena *arena = sh_arena_create(4096);
    ShJsonValue *root = NULL;
    ASSERT(sh_json_parse(out, strlen(out), arena, &root) == SH_JSON_OK);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(root, "type"), ""), "FeatureCollection");

    ShJsonValue *feat = sh_json_array_get(sh_json_get(root, "features"), 0);
    ASSERT(feat != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(feat, "id"), ""), "d1");

    ShJsonValue *geom = sh_json_get(feat, "geometry");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(geom, "type"), ""), "Point");

    ShJsonValue *coords = sh_json_get(geom, "coordinates");
    ASSERT_EQ(sh_json_array_len(coords), 2);

    ShJsonValue *pv = sh_json_get(feat, "properties");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(pv, "city"), ""), "Debrecen");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(pv, "zip"), ""), "4002");

    sh_arena_free(arena);
    free(out);
}

TEST(geojson_zero_precision)
{
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_geojson_begin(&w);
    sh_geojson_point_feature(&w, NULL, 19.0, 47.0, 0, NULL, 0);
    sh_geojson_end(&w);

    char *out = sh_json_buf_take(&jb);
    sh_json_buf_free(&jb);
    ASSERT(out != NULL);
    /* precision=0 → integer coords */
    ASSERT(strstr(out, "[19,47]") != NULL);
    free(out);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nGeoJSON Encoder Tests:\n");

    RUN_TEST(geojson_empty_collection);
    RUN_TEST(geojson_single_point);
    RUN_TEST(geojson_coordinate_order);
    RUN_TEST(geojson_precision);
    RUN_TEST(geojson_no_id);
    RUN_TEST(geojson_no_properties);
    RUN_TEST(geojson_multiple_features);
    RUN_TEST(geojson_special_chars_in_props);
    RUN_TEST(geojson_negative_coordinates);
    RUN_TEST(geojson_large_precision);
    RUN_TEST(geojson_with_json_buf);
    RUN_TEST(geojson_zero_precision);

    printf("\nGeoJSON: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
