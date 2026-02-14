/*
 * test_discover.c - Tests for auto-schema discovery
 */

#include "nx_discover.h"
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
    size_t read = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read] = '\0';
    if (out_len) *out_len = read;
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
 * Helper: parse discovered schema and look up fields
 * ============================================================================ */

static ShJsonValue *parse_schema(const char *json, size_t len, SHArena *arena)
{
    ShJsonValue *root = NULL;
    ShJsonStatus st = sh_json_parse(json, len, arena, &root);
    if (st != SH_JSON_OK) return NULL;
    return root;
}

static ShJsonValue *get_column_by_target(ShJsonValue *schema, const char *target)
{
    ShJsonValue *cols = sh_json_get(schema, "columns");
    if (!cols) return NULL;
    for (size_t i = 0; i < sh_json_array_len(cols); i++) {
        ShJsonValue *col = sh_json_array_get(cols, i);
        const char *t = sh_json_as_string(sh_json_get(col, "target"), "");
        if (strcmp(t, target) == 0) return col;
    }
    return NULL;
}

static ShJsonValue *get_validation_by_type(ShJsonValue *schema, const char *type)
{
    ShJsonValue *validate = sh_json_get(schema, "validate");
    if (!validate) return NULL;
    for (size_t i = 0; i < sh_json_array_len(validate); i++) {
        ShJsonValue *v = sh_json_array_get(validate, i);
        const char *t = sh_json_as_string(sh_json_get(v, "type"), "");
        if (strcmp(t, type) == 0) return v;
    }
    return NULL;
}

/* ============================================================================
 * Unit Tests: Error Cases
 * ============================================================================ */

TEST(null_input)
{
    ASSERT_EQ(nx_discover_schema(NULL, 0, NULL, NULL, NULL), NX_DISCOVER_ERR_NULL);
}

TEST(invalid_json)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema("not json", 8, arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_ERR_JSON);
    sh_arena_free(arena);
}

TEST(no_tables)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    const char *json = "{\"tables\":[]}";
    NxDiscoverStatus st = nx_discover_schema(json, strlen(json), arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_ERR_NO_TABLE);
    sh_arena_free(arena);
}

TEST(no_rows)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    const char *json = "{\"tables\":[{\"headers\":[\"a\"],\"rows\":[]}]}";
    NxDiscoverStatus st = nx_discover_schema(json, strlen(json), arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_ERR_NO_ROWS);
    sh_arena_free(arena);
}

TEST(status_strings)
{
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_OK), "OK");
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_ERR_NULL), "NULL input");
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_ERR_JSON), "Invalid raw JSON");
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_ERR_NO_TABLE), "No tables in raw JSON");
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_ERR_NO_ROWS), "No rows in table");
    ASSERT_STREQ(nx_discover_status_str(NX_DISCOVER_ERR_ARENA), "Arena allocation failure");
}

/* ============================================================================
 * Unit Tests: Type Inference
 * ============================================================================ */

TEST(infer_double)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"lat\",\"lon\",\"name\"],"
        "\"rows\":["
        "{\"cells\":[\"47.5\",\"19.04\",\"Budapest\"]},"
        "{\"cells\":[\"46.2\",\"20.1\",\"Szeged\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(raw, strlen(raw), arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_OK);
    ASSERT(out != NULL);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    ShJsonValue *lat_col = get_column_by_target(schema, "lat");
    ASSERT(lat_col != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(lat_col, "type"), ""), "double");

    ShJsonValue *name_col = get_column_by_target(schema, "name");
    ASSERT(name_col != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(name_col, "type"), ""), "string");

    free(out);
    sh_arena_free(arena);
}

TEST(infer_int)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"count\",\"price\"],"
        "\"rows\":["
        "{\"cells\":[\"10\",\"3.50\"]},"
        "{\"cells\":[\"20\",\"7.25\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ShJsonValue *count_col = get_column_by_target(schema, "count");
    ASSERT(count_col != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(count_col, "type"), ""), "int");

    ShJsonValue *price_col = get_column_by_target(schema, "price");
    ASSERT(price_col != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(price_col, "type"), ""), "double");

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Unit Tests: Lat/Lon Detection
 * ============================================================================ */

TEST(detect_latlon_by_header)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"Name\",\"Latitude\",\"Longitude\"],"
        "\"rows\":["
        "{\"cells\":[\"A\",\"47.5\",\"19.0\"]},"
        "{\"cells\":[\"B\",\"46.2\",\"20.1\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);

    /* Should have lat/lon columns */
    ShJsonValue *lat = get_column_by_target(schema, "lat");
    ShJsonValue *lon = get_column_by_target(schema, "lon");
    ASSERT(lat != NULL);
    ASSERT(lon != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(lat, "type"), ""), "double");
    ASSERT_STREQ(sh_json_as_string(sh_json_get(lon, "type"), ""), "double");

    /* Should have geo_bounds validation */
    ShJsonValue *geo = get_validation_by_type(schema, "geo_bounds");
    ASSERT(geo != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(detect_latlon_hungarian)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    /* Hungarian headers: szélesség = latitude, hosszúság = longitude */
    const char *raw =
        "{\"tables\":[{\"headers\":[\"Város\",\"Szélességi\",\"Hosszúsági\"],"
        "\"rows\":["
        "{\"cells\":[\"Budapest\",\"47.5\",\"19.0\"]},"
        "{\"cells\":[\"Debrecen\",\"47.5\",\"21.6\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ShJsonValue *lat = get_column_by_target(schema, "lat");
    ShJsonValue *lon = get_column_by_target(schema, "lon");
    ASSERT(lat != NULL);
    ASSERT(lon != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(detect_latlon_by_range)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    /* No obvious header names but values are in lat/lon range */
    const char *raw =
        "{\"tables\":[{\"headers\":[\"id\",\"y_coord\",\"x_coord\"],"
        "\"rows\":["
        "{\"cells\":[\"1\",\"47.5\",\"19.0\"]},"
        "{\"cells\":[\"2\",\"48.1\",\"20.3\"]},"
        "{\"cells\":[\"3\",\"46.8\",\"18.5\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    /* Adjacent double columns in lat/lon range should be detected */
    ShJsonValue *lat = get_column_by_target(schema, "lat");
    ShJsonValue *lon = get_column_by_target(schema, "lon");
    ASSERT(lat != NULL);
    ASSERT(lon != NULL);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Unit Tests: Uniqueness & Row ID
 * ============================================================================ */

TEST(unique_column_detected)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"code\",\"value\"],"
        "\"rows\":["
        "{\"cells\":[\"A001\",\"10\"]},"
        "{\"cells\":[\"A002\",\"20\"]},"
        "{\"cells\":[\"A003\",\"30\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);

    /* Should have unique validation on 'code' */
    ShJsonValue *uniq = get_validation_by_type(schema, "unique");
    ASSERT(uniq != NULL);
    ShJsonValue *fields = sh_json_get(uniq, "fields");
    ASSERT(fields != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_array_get(fields, 0), ""), "code");

    /* Should have row_id based on 'code' */
    ShJsonValue *row_id = sh_json_get(schema, "row_id");
    ASSERT(row_id != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(row_id, "template"), ""), "{code}");
    ASSERT_EQ(sh_json_as_bool(sh_json_get(row_id, "slugify"), false), true);

    free(out);
    sh_arena_free(arena);
}

TEST(non_unique_no_validation)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"category\",\"amount\"],"
        "\"rows\":["
        "{\"cells\":[\"food\",\"10\"]},"
        "{\"cells\":[\"food\",\"20\"]},"
        "{\"cells\":[\"drink\",\"15\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);

    /* category is not unique, so no unique validation */
    ShJsonValue *uniq = get_validation_by_type(schema, "unique");
    ASSERT(uniq == NULL);

    /* No row_id since no unique string column */
    ShJsonValue *row_id = sh_json_get(schema, "row_id");
    ASSERT(row_id == NULL);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Unit Tests: Transforms
 * ============================================================================ */

TEST(tilde_transform)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"name\",\"lat\",\"lon\"],"
        "\"rows\":["
        "{\"cells\":[\"Depot A\",\"~47.5\",\"~19.0\"]},"
        "{\"cells\":[\"Depot B\",\"~46.2\",\"~20.1\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ShJsonValue *lat = get_column_by_target(schema, "lat");
    ASSERT(lat != NULL);

    /* lat should have transforms with replace ["~",""] */
    ShJsonValue *transforms = sh_json_get(lat, "transforms");
    ASSERT(transforms != NULL);
    ASSERT(sh_json_array_len(transforms) > 0);

    free(out);
    sh_arena_free(arena);
}

TEST(required_detection)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"name\",\"optional\"],"
        "\"rows\":["
        "{\"cells\":[\"A\",\"x\"]},"
        "{\"cells\":[\"B\",\"\"]},"
        "{\"cells\":[\"C\",\"z\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);

    /* 'name' should be required (all non-empty) */
    ShJsonValue *name = get_column_by_target(schema, "name");
    ASSERT(name != NULL);
    ASSERT_EQ(sh_json_as_bool(sh_json_get(name, "required"), false), true);

    /* 'optional' should NOT be required (has empty value) */
    ShJsonValue *opt = get_column_by_target(schema, "optional");
    ASSERT(opt != NULL);
    /* required key should not be present */
    ASSERT(sh_json_get(opt, "required") == NULL);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Unit Tests: Schema Usability
 * ============================================================================ */

TEST(discovered_schema_transforms)
{
    /* Verify that a discovered schema can actually drive the transform engine */
    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"city\",\"lat\",\"lon\"],"
        "\"rows\":["
        "{\"cells\":[\"Budapest\",\"47.497\",\"19.040\"]},"
        "{\"cells\":[\"Debrecen\",\"47.531\",\"21.624\"]}"
        "]}]}";

    /* Discover schema */
    char *schema_json = NULL; size_t schema_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &schema_json, &schema_len), NX_DISCOVER_OK);

    /* Transform with discovered schema */
    char *canon = NULL; size_t canon_len = 0;
    NxXformStatus xst = nx_xform_apply(raw, strlen(raw), schema_json, schema_len,
                                        arena, NULL, &canon, &canon_len);
    ASSERT_EQ(xst, NX_XFORM_OK);
    ASSERT(canon != NULL);

    /* Parse result */
    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *result = parse_schema(canon, canon_len, arena2);
    ASSERT(result != NULL);

    ShJsonValue *records = sh_json_get(result, "records");
    ASSERT(records != NULL);
    ASSERT_EQ(sh_json_array_len(records), (size_t)2);

    /* First record should have lat/lon as doubles */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lat"), 0), 47.497, 0.001);
    ASSERT_NEAR(sh_json_as_double(sh_json_get(r0, "lon"), 0), 19.040, 0.001);

    free(schema_json);
    free(canon);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Unit Tests: Geo Bounds
 * ============================================================================ */

TEST(geo_bounds_padding)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"name\",\"lat\",\"lon\"],"
        "\"rows\":["
        "{\"cells\":[\"A\",\"46.0\",\"18.0\"]},"
        "{\"cells\":[\"B\",\"48.0\",\"22.0\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ShJsonValue *geo = get_validation_by_type(schema, "geo_bounds");
    ASSERT(geo != NULL);

    ShJsonValue *bounds = sh_json_get(geo, "bounds");
    ASSERT(bounds != NULL);

    /* lat range: 46-48, 5% pad = 0.1 -> min_lat ~= 45.9, max_lat ~= 48.1 */
    double min_lat = sh_json_as_double(sh_json_get(bounds, "min_lat"), 0);
    double max_lat = sh_json_as_double(sh_json_get(bounds, "max_lat"), 0);
    ASSERT(min_lat < 46.0);
    ASSERT(max_lat > 48.0);

    /* lon range: 18-22, 5% pad = 0.2 -> min_lon ~= 17.8, max_lon ~= 22.2 */
    double min_lon = sh_json_as_double(sh_json_get(bounds, "min_lon"), 0);
    double max_lon = sh_json_as_double(sh_json_get(bounds, "max_lon"), 0);
    ASSERT(min_lon < 18.0);
    ASSERT(max_lon > 22.0);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Unit Tests: Header Slugification
 * ============================================================================ */

TEST(header_slugify)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"First Name\",\"Last-Name\",\"ZIP Code\"],"
        "\"rows\":["
        "{\"cells\":[\"John\",\"Doe\",\"1234\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);

    /* "First Name" -> "first_name" */
    ASSERT(get_column_by_target(schema, "first_name") != NULL);
    /* "Last-Name" -> "last_name" */
    ASSERT(get_column_by_target(schema, "last_name") != NULL);
    /* "ZIP Code" -> "zip_code" */
    ASSERT(get_column_by_target(schema, "zip_code") != NULL);

    free(out);
    sh_arena_free(arena);
}

TEST(duplicate_header_dedup)
{
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"Value\",\"Value\",\"Name\"],"
        "\"rows\":["
        "{\"cells\":[\"10\",\"20\",\"A\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ShJsonValue *cols = sh_json_get(schema, "columns");
    ASSERT(cols != NULL);
    ASSERT_EQ(sh_json_array_len(cols), (size_t)3);

    /* First "Value" stays as "value" */
    const char *t0 = sh_json_as_string(sh_json_get(sh_json_array_get(cols, 0), "target"), "");
    ASSERT_STREQ(t0, "value");

    /* Second "Value" becomes "value_1" */
    const char *t1 = sh_json_as_string(sh_json_get(sh_json_array_get(cols, 1), "target"), "");
    ASSERT_STREQ(t1, "value_1");

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Golden Tests: XLSX01 (GLS Hungary Depots)
 * ============================================================================ */

static void test_golden_xlsx01_discover(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "XLSX01_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(raw, raw_len, arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_OK);
    ASSERT(out != NULL);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* XLSX01 has 7 columns */
    ShJsonValue *cols = sh_json_get(schema, "columns");
    ASSERT(cols != NULL);
    ASSERT_EQ(sh_json_array_len(cols), (size_t)7);

    /* Should detect lat/lon */
    ShJsonValue *lat = get_column_by_target(schema, "lat");
    ShJsonValue *lon = get_column_by_target(schema, "lon");
    ASSERT(lat != NULL);
    ASSERT(lon != NULL);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(lat, "type"), ""), "double");

    /* Should have geo_bounds validation */
    ShJsonValue *geo = get_validation_by_type(schema, "geo_bounds");
    ASSERT(geo != NULL);

    /* Bounds should roughly cover Hungary */
    ShJsonValue *bounds = sh_json_get(geo, "bounds");
    double min_lat = sh_json_as_double(sh_json_get(bounds, "min_lat"), 0);
    double max_lat = sh_json_as_double(sh_json_get(bounds, "max_lat"), 0);
    ASSERT(min_lat > 45.0 && min_lat < 47.0);
    ASSERT(max_lat > 47.5 && max_lat < 49.0);

    free(out);
    free(raw);
    sh_arena_free(arena);
}

static void test_golden_xlsx01_discover_transform(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "XLSX01_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);

    /* Discover */
    char *schema = NULL; size_t schema_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, raw_len, arena, &schema, &schema_len), NX_DISCOVER_OK);

    /* Transform */
    char *canon = NULL; size_t canon_len = 0;
    NxXformStatus xst = nx_xform_apply(raw, raw_len, schema, schema_len,
                                        arena, NULL, &canon, &canon_len);
    ASSERT_EQ(xst, NX_XFORM_OK);

    /* Parse canonical output */
    SHArena *arena2 = sh_arena_create(1024 * 1024);
    ShJsonValue *result = parse_schema(canon, canon_len, arena2);
    ASSERT(result != NULL);

    ShJsonValue *records = sh_json_get(result, "records");
    ASSERT(records != NULL);
    ASSERT_EQ(sh_json_array_len(records), (size_t)7); /* XLSX01 has 7 rows */

    /* Check that lat/lon are proper doubles */
    ShJsonValue *r0 = sh_json_array_get(records, 0);
    double lat0 = sh_json_as_double(sh_json_get(r0, "lat"), 0);
    double lon0 = sh_json_as_double(sh_json_get(r0, "lon"), 0);
    ASSERT(lat0 > 45.0 && lat0 < 49.0);
    ASSERT(lon0 > 16.0 && lon0 < 23.0);

    free(schema);
    free(canon);
    free(raw);
    sh_arena_free(arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Golden Tests: PDF01 (GLS Hungary Automata)
 * ============================================================================ */

static void test_golden_pdf01_discover(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF01_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(raw, raw_len, arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* PDF01 has 18 columns */
    ShJsonValue *cols = sh_json_get(schema, "columns");
    ASSERT(cols != NULL);
    ASSERT_EQ(sh_json_array_len(cols), (size_t)18);

    /* PDF01 GPS columns have non-standard format (spaces in numbers,
     * lat values like "01607") so lat/lon won't be auto-detected.
     * That's correct behavior - discovery is conservative. */
    ASSERT_EQ(sh_json_as_int(sh_json_get(schema, "nx_schema"), 0), 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(schema, "version"), ""), "auto-discovered");

    free(out);
    free(raw);
    sh_arena_free(arena);
}

static void test_golden_pdf01_discover_transform(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF01_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(16 * 1024 * 1024);

    /* Discover */
    char *schema = NULL; size_t schema_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, raw_len, arena, &schema, &schema_len), NX_DISCOVER_OK);

    /* Transform with discovered schema */
    SHArena *xform_arena = sh_arena_create(16 * 1024 * 1024);
    char *canon = NULL; size_t canon_len = 0;
    NxXformStatus xst = nx_xform_apply(raw, raw_len, schema, schema_len,
                                        xform_arena, NULL, &canon, &canon_len);
    ASSERT_EQ(xst, NX_XFORM_OK);

    /* Parse and check record count (PDF01 has ~689 rows) */
    SHArena *arena2 = sh_arena_create(4 * 1024 * 1024);
    ShJsonValue *result = parse_schema(canon, canon_len, arena2);
    ASSERT(result != NULL);

    ShJsonValue *records = sh_json_get(result, "records");
    ASSERT(records != NULL);
    size_t nrec = sh_json_array_len(records);
    ASSERT(nrec > 600); /* Should have 600+ records */

    free(schema);
    free(canon);
    free(raw);
    sh_arena_free(arena);
    sh_arena_free(xform_arena);
    sh_arena_free(arena2);
}

/* ============================================================================
 * Unit Tests: Continuation Row Detection
 * ============================================================================ */

TEST(discover_continuation_detection)
{
    /* Synthetic data with 20% continuation rows — should emit row_merge */
    SHArena *arena = sh_arena_create(1024 * 1024);

    /* Build raw JSON: 8 data rows + 2 continuation rows = 10 total (20%) */
    const char *raw =
        "{\"tables\":[{\"headers\":[\"ZIP\",\"City\",\"Name\",\"Hours\"],"
        "\"rows\":["
        "{\"cells\":[\"1111\",\"A\",\"S1\",\"H-P\"]},"
        "{\"cells\":[\"2222\",\"B\",\"S2\",\"H-P\"]},"
        "{\"cells\":[\"\",\"\",\"\",\"Szo\"]},"
        "{\"cells\":[\"3333\",\"C\",\"S3\",\"H-P\"]},"
        "{\"cells\":[\"4444\",\"D\",\"S4\",\"H-P\"]},"
        "{\"cells\":[\"\",\"\",\"\",\"V\"]},"
        "{\"cells\":[\"5555\",\"E\",\"S5\",\"H-P\"]},"
        "{\"cells\":[\"6666\",\"F\",\"S6\",\"H-P\"]},"
        "{\"cells\":[\"7777\",\"G\",\"S7\",\"H-P\"]},"
        "{\"cells\":[\"8888\",\"H\",\"S8\",\"H-P\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* Should have row_merge config */
    ShJsonValue *rm = sh_json_get(schema, "row_merge");
    ASSERT(rm != NULL);

    ShJsonValue *keys = sh_json_get(rm, "key_columns");
    ASSERT(keys != NULL);
    ASSERT(sh_json_array_len(keys) > 0);

    ASSERT_STREQ(sh_json_as_string(sh_json_get(rm, "separator"), ""), " ");

    free(out);
    sh_arena_free(arena);
}

TEST(discover_no_continuation)
{
    /* All rows have non-empty key columns — 0% continuation, no row_merge */
    SHArena *arena = sh_arena_create(1024 * 1024);
    const char *raw =
        "{\"tables\":[{\"headers\":[\"ZIP\",\"City\",\"Name\"],"
        "\"rows\":["
        "{\"cells\":[\"1111\",\"A\",\"X\"]},"
        "{\"cells\":[\"2222\",\"B\",\"Y\"]},"
        "{\"cells\":[\"3333\",\"C\",\"Z\"]}"
        "]}]}";

    char *out = NULL; size_t out_len = 0;
    ASSERT_EQ(nx_discover_schema(raw, strlen(raw), arena, &out, &out_len), NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* Should NOT have row_merge */
    ShJsonValue *rm = sh_json_get(schema, "row_merge");
    ASSERT(rm == NULL);

    free(out);
    sh_arena_free(arena);
}

/* ============================================================================
 * Golden Tests: PDF02 (GLS Hungary PuDo)
 * ============================================================================ */

static void test_golden_pdf02_discover(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(raw, raw_len, arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* PDF02 is a PuDo list: ZIP, city, name, address, hours, phone,
     * pickup time, notes.  No GPS data — lat/lon should NOT be detected. */
    ShJsonValue *cols = sh_json_get(schema, "columns");
    ASSERT(cols != NULL);
    ASSERT_EQ(sh_json_array_len(cols), (size_t)8);

    /* No geo_bounds since no lat/lon */
    ShJsonValue *geo = get_validation_by_type(schema, "geo_bounds");
    ASSERT(geo == NULL);

    /* Schema metadata */
    ASSERT_EQ(sh_json_as_int(sh_json_get(schema, "nx_schema"), 0), 1);
    ASSERT_STREQ(sh_json_as_string(sh_json_get(schema, "version"), ""), "auto-discovered");

    free(out);
    free(raw);
    sh_arena_free(arena);
}

static void test_golden_pdf02_discover_row_merge(int *skip)
{
    size_t raw_len = 0;
    char *raw = read_golden_file(GOLDEN_DIR "PDF02_raw.json", &raw_len);
    if (!raw) { *skip = 1; return; }

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    char *out = NULL; size_t out_len = 0;
    NxDiscoverStatus st = nx_discover_schema(raw, raw_len, arena, &out, &out_len);
    ASSERT_EQ(st, NX_DISCOVER_OK);

    ShJsonValue *schema = parse_schema(out, out_len, arena);
    ASSERT(schema != NULL);

    /* PDF02 has ~18% continuation rows — should detect row_merge */
    ShJsonValue *rm = sh_json_get(schema, "row_merge");
    ASSERT(rm != NULL);

    ShJsonValue *keys = sh_json_get(rm, "key_columns");
    ASSERT(keys != NULL);
    ASSERT(sh_json_array_len(keys) > 0);

    free(out);
    free(raw);
    sh_arena_free(arena);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nSchema Discovery Tests:\n\n");

    printf("  Error Cases:\n");
    RUN_TEST(null_input);
    RUN_TEST(invalid_json);
    RUN_TEST(no_tables);
    RUN_TEST(no_rows);
    RUN_TEST(status_strings);

    printf("\n  Type Inference:\n");
    RUN_TEST(infer_double);
    RUN_TEST(infer_int);

    printf("\n  Lat/Lon Detection:\n");
    RUN_TEST(detect_latlon_by_header);
    RUN_TEST(detect_latlon_hungarian);
    RUN_TEST(detect_latlon_by_range);

    printf("\n  Uniqueness & Row ID:\n");
    RUN_TEST(unique_column_detected);
    RUN_TEST(non_unique_no_validation);

    printf("\n  Transforms:\n");
    RUN_TEST(tilde_transform);
    RUN_TEST(required_detection);

    printf("\n  Schema Usability:\n");
    RUN_TEST(discovered_schema_transforms);
    RUN_TEST(geo_bounds_padding);

    printf("\n  Header Processing:\n");
    RUN_TEST(header_slugify);
    RUN_TEST(duplicate_header_dedup);

    printf("\n  Continuation Detection:\n");
    RUN_TEST(discover_continuation_detection);
    RUN_TEST(discover_no_continuation);

    printf("\n  Golden Tests (real-world files):\n");
    RUN_GOLDEN(xlsx01_discover);
    RUN_GOLDEN(xlsx01_discover_transform);
    RUN_GOLDEN(pdf01_discover);
    RUN_GOLDEN(pdf01_discover_transform);
    RUN_GOLDEN(pdf02_discover);
    RUN_GOLDEN(pdf02_discover_row_merge);

    printf("\nSchema Discovery: %d passed, %d total", tests_passed, tests_run);
    if (golden_skipped > 0) printf(" (%d golden skipped)", golden_skipped);
    printf("\n");
    return tests_passed == tests_run ? 0 : 1;
}
