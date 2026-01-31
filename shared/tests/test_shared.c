/*
 * test_shared.c - Tests for shared library
 */

#include "shared.h"
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
    printf("  %-50s ", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while (0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n", #cond); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))

/* ============================================================================
 * Version Tests
 * ============================================================================ */

TEST(version)
{
    const char *v = sh_version();
    ASSERT(v != NULL);
    ASSERT(strlen(v) > 0);
}

/* ============================================================================
 * Haversine Tests
 * ============================================================================ */

TEST(haversine_same_point)
{
    SHCoord a = {47.4979, 19.0402};  /* Budapest */
    double dist = sh_haversine(a, a);
    ASSERT_NEAR(dist, 0.0, 0.001);
}

TEST(haversine_known_distance)
{
    /* Budapest to Vienna: ~215 km */
    SHCoord budapest = {47.4979, 19.0402};
    SHCoord vienna = {48.2082, 16.3738};
    double dist = sh_haversine(budapest, vienna);
    ASSERT_NEAR(dist, 215000, 5000);  /* Within 5km */
}

TEST(haversine_antipodal)
{
    /* Nearly antipodal points: ~20000 km */
    SHCoord a = {0, 0};
    SHCoord b = {0, 179.9};
    double dist = sh_haversine(a, b);
    ASSERT(dist > 19900000);  /* > 19900 km */
}

TEST(distance_fast_same_point)
{
    SHCoord a = {47.4979, 19.0402};
    double dist = sh_distance_fast(a, a);
    ASSERT_NEAR(dist, 0.0, 0.001);
}

TEST(distance_fast_short)
{
    /* Short distance: fast approximation should be accurate */
    SHCoord a = {47.4979, 19.0402};
    SHCoord b = {47.5000, 19.0500};  /* ~1km */
    double exact = sh_haversine(a, b);
    double fast = sh_distance_fast(a, b);
    ASSERT_NEAR(fast, exact, exact * 0.01);  /* Within 1% */
}

TEST(haversine_fixed)
{
    /* Using SH_COORD_SCALE (1e7) for fixed-point coordinates */
    SHCoordFixed a = {(int32_t)(47.4979 * SH_COORD_SCALE), (int32_t)(19.0402 * SH_COORD_SCALE)};
    SHCoordFixed b = {(int32_t)(48.2082 * SH_COORD_SCALE), (int32_t)(16.3738 * SH_COORD_SCALE)};
    double dist = sh_haversine_fixed(a, b);
    ASSERT_NEAR(dist, 215000, 5000);
}

/* ============================================================================
 * Coordinate Utility Tests
 * ============================================================================ */

TEST(coord_valid)
{
    SHCoord valid = {45.0, 90.0};
    SHCoord invalid_lat = {91.0, 0.0};
    SHCoord invalid_lon = {0.0, 181.0};

    ASSERT(sh_coord_valid(valid));
    ASSERT(!sh_coord_valid(invalid_lat));
    ASSERT(!sh_coord_valid(invalid_lon));
}

TEST(coord_in_bbox)
{
    SHBBox bbox = {.min_lat = 46, .max_lat = 48, .min_lon = 18, .max_lon = 20};
    SHCoord inside = {47.0, 19.0};
    SHCoord outside = {50.0, 19.0};

    ASSERT(sh_coord_in_bbox(inside, bbox));
    ASSERT(!sh_coord_in_bbox(outside, bbox));
}

TEST(coord_midpoint)
{
    SHCoord a = {0.0, 0.0};
    SHCoord b = {10.0, 20.0};
    SHCoord mid = sh_coord_midpoint(a, b);

    ASSERT_NEAR(mid.lat, 5.0, 0.001);
    ASSERT_NEAR(mid.lon, 10.0, 0.001);
}

TEST(bearing_east)
{
    SHCoord a = {0.0, 0.0};
    SHCoord b = {0.0, 1.0};  /* 1 degree east */
    double bearing = sh_bearing(a, b);
    ASSERT_NEAR(bearing, 90.0, 0.1);  /* Should be ~90 degrees (east) */
}

TEST(bearing_north)
{
    SHCoord a = {0.0, 0.0};
    SHCoord b = {1.0, 0.0};  /* 1 degree north */
    double bearing = sh_bearing(a, b);
    ASSERT_NEAR(bearing, 0.0, 0.1);  /* Should be ~0 degrees (north) */
}

TEST(destination)
{
    SHCoord start = {0.0, 0.0};
    SHCoord dest = sh_destination(start, 90.0, 111000);  /* ~1 degree east */
    ASSERT_NEAR(dest.lat, 0.0, 0.01);
    ASSERT_NEAR(dest.lon, 1.0, 0.1);
}

/* ============================================================================
 * Bounding Box Tests
 * ============================================================================ */

TEST(bbox_init)
{
    SHBBox bbox;
    sh_bbox_init(&bbox);
    ASSERT(!sh_bbox_valid(bbox));  /* Empty bbox is invalid */
}

TEST(bbox_expand)
{
    SHBBox bbox;
    sh_bbox_init(&bbox);

    SHCoord c1 = {10.0, 20.0};
    SHCoord c2 = {30.0, 40.0};

    sh_bbox_expand(&bbox, c1);
    sh_bbox_expand(&bbox, c2);

    ASSERT(sh_bbox_valid(bbox));
    ASSERT_NEAR(bbox.min_lat, 10.0, 0.001);
    ASSERT_NEAR(bbox.max_lat, 30.0, 0.001);
    ASSERT_NEAR(bbox.min_lon, 20.0, 0.001);
    ASSERT_NEAR(bbox.max_lon, 40.0, 0.001);
}

TEST(bbox_intersects)
{
    SHBBox a = {.min_lat = 0, .max_lat = 10, .min_lon = 0, .max_lon = 10};
    SHBBox b = {.min_lat = 5, .max_lat = 15, .min_lon = 5, .max_lon = 15};
    SHBBox c = {.min_lat = 20, .max_lat = 30, .min_lon = 20, .max_lon = 30};

    ASSERT(sh_bbox_intersects(a, b));
    ASSERT(!sh_bbox_intersects(a, c));
}

TEST(bbox_union)
{
    SHBBox a = {.min_lat = 0, .max_lat = 10, .min_lon = 0, .max_lon = 10};
    SHBBox b = {.min_lat = 5, .max_lat = 15, .min_lon = 5, .max_lon = 15};
    SHBBox u = sh_bbox_union(a, b);

    ASSERT_NEAR(u.min_lat, 0.0, 0.001);
    ASSERT_NEAR(u.max_lat, 15.0, 0.001);
    ASSERT_NEAR(u.min_lon, 0.0, 0.001);
    ASSERT_NEAR(u.max_lon, 15.0, 0.001);
}

/* ============================================================================
 * Web Mercator Tests
 * ============================================================================ */

TEST(latlon_to_mercator_origin)
{
    double x, y;
    sh_latlon_to_mercator(0, 0, &x, &y);
    ASSERT_NEAR(x, 0.0, 0.1);
    ASSERT_NEAR(y, 0.0, 0.1);
}

TEST(mercator_roundtrip)
{
    double lat_in = 47.4979;
    double lon_in = 19.0402;
    double x, y, lat_out, lon_out;

    sh_latlon_to_mercator(lat_in, lon_in, &x, &y);
    sh_mercator_to_latlon(x, y, &lat_out, &lon_out);

    ASSERT_NEAR(lat_out, lat_in, 0.0001);
    ASSERT_NEAR(lon_out, lon_in, 0.0001);
}

TEST(latlon_to_tile_z0)
{
    int tx, ty;
    sh_latlon_to_tile(0, 0, 0, &tx, &ty);
    ASSERT_EQ(tx, 0);
    ASSERT_EQ(ty, 0);
}

TEST(latlon_to_tile_budapest)
{
    int tx, ty;
    /* Budapest at zoom 14 */
    sh_latlon_to_tile(47.4979, 19.0402, 14, &tx, &ty);
    ASSERT_EQ(tx, 9058);
    ASSERT_EQ(ty, 5729);
}

TEST(tile_bounds_z0)
{
    SHBBox bbox = sh_tile_bounds(0, 0, 0);
    ASSERT_NEAR(bbox.min_lon, -180.0, 0.001);
    ASSERT_NEAR(bbox.max_lon, 180.0, 0.001);
    ASSERT_NEAR(bbox.max_lat, 85.05, 0.1);
    ASSERT_NEAR(bbox.min_lat, -85.05, 0.1);
}

TEST(tile_bounds_z1)
{
    /* Top-left tile at z=1 */
    SHBBox bbox = sh_tile_bounds(1, 0, 0);
    ASSERT_NEAR(bbox.min_lon, -180.0, 0.001);
    ASSERT_NEAR(bbox.max_lon, 0.0, 0.001);
    ASSERT_NEAR(bbox.max_lat, 85.05, 0.1);
    ASSERT_NEAR(bbox.min_lat, 0.0, 0.1);
}

/* ============================================================================
 * Protobuf Tests
 * ============================================================================ */

TEST(pb_varint_small)
{
    uint8_t buf[10];
    uint64_t value;

    /* Encode and decode small value (1 byte) */
    int n = sh_pb_write_varint(buf, sizeof(buf), 127);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(buf[0], 127);

    int m = sh_pb_read_varint(buf, n, &value);
    ASSERT_EQ(m, 1);
    ASSERT_EQ(value, 127);
}

TEST(pb_varint_300)
{
    uint8_t buf[10];
    uint64_t value;

    /* Encode and decode 300 (2 bytes) */
    int n = sh_pb_write_varint(buf, sizeof(buf), 300);
    ASSERT_EQ(n, 2);

    int m = sh_pb_read_varint(buf, n, &value);
    ASSERT_EQ(m, 2);
    ASSERT_EQ(value, 300);
}

TEST(pb_varint_large)
{
    uint8_t buf[10];
    uint64_t value;

    /* Encode and decode large value */
    uint64_t large = 0x123456789ABCDEF0ULL;
    int n = sh_pb_write_varint(buf, sizeof(buf), large);
    ASSERT(n > 0);

    int m = sh_pb_read_varint(buf, n, &value);
    ASSERT_EQ(m, n);
    ASSERT_EQ(value, large);
}

TEST(pb_svarint_positive)
{
    uint8_t buf[10];
    int64_t value;

    /* Encode and decode positive signed varint */
    int n = sh_pb_write_svarint(buf, sizeof(buf), 100);
    ASSERT(n > 0);

    int m = sh_pb_read_svarint(buf, n, &value);
    ASSERT_EQ(m, n);
    ASSERT_EQ(value, 100);
}

TEST(pb_svarint_negative)
{
    uint8_t buf[10];
    int64_t value;

    /* Encode and decode negative signed varint */
    int n = sh_pb_write_svarint(buf, sizeof(buf), -100);
    ASSERT(n > 0);

    int m = sh_pb_read_svarint(buf, n, &value);
    ASSERT_EQ(m, n);
    ASSERT_EQ(value, -100);
}

TEST(pb_svarint_edge)
{
    uint8_t buf[10];
    int64_t value;

    /* Edge case: -1 (zigzag encodes to 1) */
    int n = sh_pb_write_svarint(buf, sizeof(buf), -1);
    ASSERT_EQ(n, 1);

    int m = sh_pb_read_svarint(buf, n, &value);
    ASSERT_EQ(value, -1);
}

TEST(pb_tag_roundtrip)
{
    uint8_t buf[10];
    uint32_t field, wire;

    /* Write and read field 15 with wire type 2 (length-delimited) */
    int n = sh_pb_write_tag(buf, sizeof(buf), 15, SH_PB_WIRE_LENGTH_DELIM);
    ASSERT(n > 0);

    int m = sh_pb_read_tag(buf, n, &field, &wire);
    ASSERT_EQ(m, n);
    ASSERT_EQ(field, 15);
    ASSERT_EQ(wire, SH_PB_WIRE_LENGTH_DELIM);
}

TEST(pb_fixed32_roundtrip)
{
    uint8_t buf[4];
    uint32_t value;

    int n = sh_pb_write_fixed32(buf, sizeof(buf), 0xDEADBEEF);
    ASSERT_EQ(n, 4);

    int m = sh_pb_read_fixed32(buf, n, &value);
    ASSERT_EQ(m, 4);
    ASSERT_EQ(value, 0xDEADBEEF);
}

TEST(pb_fixed64_roundtrip)
{
    uint8_t buf[8];
    uint64_t value;

    int n = sh_pb_write_fixed64(buf, sizeof(buf), 0xDEADBEEFCAFEBABEULL);
    ASSERT_EQ(n, 8);

    int m = sh_pb_read_fixed64(buf, n, &value);
    ASSERT_EQ(m, 8);
    ASSERT_EQ(value, 0xDEADBEEFCAFEBABEULL);
}

TEST(pb_packed_svarint)
{
    /* Create packed array: [-1, 0, 1, 100, -100] */
    uint8_t buf[32];
    int pos = 0;
    pos += sh_pb_write_svarint(buf + pos, sizeof(buf) - pos, -1);
    pos += sh_pb_write_svarint(buf + pos, sizeof(buf) - pos, 0);
    pos += sh_pb_write_svarint(buf + pos, sizeof(buf) - pos, 1);
    pos += sh_pb_write_svarint(buf + pos, sizeof(buf) - pos, 100);
    pos += sh_pb_write_svarint(buf + pos, sizeof(buf) - pos, -100);

    int64_t out[10];
    size_t count = sh_pb_read_packed_svarint_array(buf, pos, out, 10);
    ASSERT_EQ(count, 5);
    ASSERT_EQ(out[0], -1);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 1);
    ASSERT_EQ(out[3], 100);
    ASSERT_EQ(out[4], -100);
}

TEST(pb_delta_decode)
{
    int64_t arr[] = {100, 5, 10, -3, 7};
    sh_pb_delta_decode_i64(arr, 5);

    /* After delta decode: 100, 105, 115, 112, 119 */
    ASSERT_EQ(arr[0], 100);
    ASSERT_EQ(arr[1], 105);
    ASSERT_EQ(arr[2], 115);
    ASSERT_EQ(arr[3], 112);
    ASSERT_EQ(arr[4], 119);
}

TEST(pb_skip_field_varint)
{
    uint8_t buf[] = {0xAC, 0x02};  /* 300 as varint */
    int n = sh_pb_skip_field(buf, sizeof(buf), SH_PB_WIRE_VARINT);
    ASSERT_EQ(n, 2);
}

TEST(pb_skip_field_fixed)
{
    uint8_t buf[8] = {0};
    ASSERT_EQ(sh_pb_skip_field(buf, 8, SH_PB_WIRE_FIXED32), 4);
    ASSERT_EQ(sh_pb_skip_field(buf, 8, SH_PB_WIRE_FIXED64), 8);
}

/* ============================================================================
 * Inflate/Deflate Tests
 * ============================================================================ */

TEST(inflate_deflate_roundtrip)
{
    const char *text = "Hello, World! This is a test of zlib compression.";
    size_t text_len = strlen(text);

    /* Compress */
    uint8_t compressed[256];
    size_t compressed_len;
    SHStatus status = sh_deflate((const uint8_t *)text, text_len,
                                 compressed, sizeof(compressed),
                                 &compressed_len, 6);
    ASSERT_EQ(status, SH_OK);
    ASSERT(compressed_len > 0);
    ASSERT(compressed_len < text_len + 20);  /* Some overhead OK */

    /* Decompress */
    uint8_t decompressed[256];
    size_t decompressed_len;
    status = sh_inflate(compressed, compressed_len,
                        decompressed, sizeof(decompressed),
                        &decompressed_len);
    ASSERT_EQ(status, SH_OK);
    ASSERT_EQ(decompressed_len, text_len);
    ASSERT(memcmp(decompressed, text, text_len) == 0);
}

TEST(inflate_alloc)
{
    const char *text = "Testing sh_inflate_alloc function.";
    size_t text_len = strlen(text);

    /* Compress */
    uint8_t compressed[256];
    size_t compressed_len;
    SHStatus status = sh_deflate((const uint8_t *)text, text_len,
                                 compressed, sizeof(compressed),
                                 &compressed_len, 6);
    ASSERT_EQ(status, SH_OK);

    /* Decompress with alloc */
    size_t actual_len;
    uint8_t *decompressed = sh_inflate_alloc(compressed, compressed_len,
                                             text_len, &actual_len);
    ASSERT(decompressed != NULL);
    ASSERT_EQ(actual_len, text_len);
    ASSERT(memcmp(decompressed, text, text_len) == 0);
    free(decompressed);
}

/* ============================================================================
 * String Table Tests
 * ============================================================================ */

TEST(string_table_basic)
{
    SHStringTable st;
    sh_string_table_init(&st);

    ASSERT_EQ(st.count, 0);

    /* Add some strings */
    SHStatus status = sh_string_table_add(&st, (const uint8_t *)"hello", 5);
    ASSERT_EQ(status, SH_OK);

    status = sh_string_table_add(&st, (const uint8_t *)"world", 5);
    ASSERT_EQ(status, SH_OK);

    status = sh_string_table_add(&st, (const uint8_t *)"", 0);  /* Empty string */
    ASSERT_EQ(status, SH_OK);

    ASSERT_EQ(st.count, 3);

    /* Get strings */
    ASSERT(strcmp(sh_string_table_get(&st, 0), "hello") == 0);
    ASSERT(strcmp(sh_string_table_get(&st, 1), "world") == 0);
    ASSERT(strcmp(sh_string_table_get(&st, 2), "") == 0);

    /* Out of bounds returns empty string */
    ASSERT(strcmp(sh_string_table_get(&st, 999), "") == 0);

    sh_string_table_free(&st);
    ASSERT_EQ(st.count, 0);
}

TEST(string_table_large)
{
    SHStringTable st;
    sh_string_table_init(&st);

    /* Add many strings to trigger reallocation */
    for (int i = 0; i < 1000; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "string%d", i);
        SHStatus status = sh_string_table_add(&st, (const uint8_t *)buf, len);
        ASSERT_EQ(status, SH_OK);
    }

    ASSERT_EQ(st.count, 1000);

    /* Verify some */
    ASSERT(strcmp(sh_string_table_get(&st, 0), "string0") == 0);
    ASSERT(strcmp(sh_string_table_get(&st, 500), "string500") == 0);
    ASSERT(strcmp(sh_string_table_get(&st, 999), "string999") == 0);

    sh_string_table_free(&st);
}

/* ============================================================================
 * Block Header Tests
 * ============================================================================ */

TEST(block_header_init)
{
    SHBlockHeader header;
    sh_block_header_init(&header);

    ASSERT_EQ(header.granularity, 100);
    ASSERT_EQ(header.lat_offset, 0);
    ASSERT_EQ(header.lon_offset, 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\n=== Shared Library Tests ===\n\n");

    printf("Version:\n");
    RUN_TEST(version);

    printf("\nHaversine Distance:\n");
    RUN_TEST(haversine_same_point);
    RUN_TEST(haversine_known_distance);
    RUN_TEST(haversine_antipodal);
    RUN_TEST(distance_fast_same_point);
    RUN_TEST(distance_fast_short);
    RUN_TEST(haversine_fixed);

    printf("\nCoordinate Utilities:\n");
    RUN_TEST(coord_valid);
    RUN_TEST(coord_in_bbox);
    RUN_TEST(coord_midpoint);
    RUN_TEST(bearing_east);
    RUN_TEST(bearing_north);
    RUN_TEST(destination);

    printf("\nBounding Box:\n");
    RUN_TEST(bbox_init);
    RUN_TEST(bbox_expand);
    RUN_TEST(bbox_intersects);
    RUN_TEST(bbox_union);

    printf("\nWeb Mercator:\n");
    RUN_TEST(latlon_to_mercator_origin);
    RUN_TEST(mercator_roundtrip);
    RUN_TEST(latlon_to_tile_z0);
    RUN_TEST(latlon_to_tile_budapest);
    RUN_TEST(tile_bounds_z0);
    RUN_TEST(tile_bounds_z1);

    printf("\nProtobuf:\n");
    RUN_TEST(pb_varint_small);
    RUN_TEST(pb_varint_300);
    RUN_TEST(pb_varint_large);
    RUN_TEST(pb_svarint_positive);
    RUN_TEST(pb_svarint_negative);
    RUN_TEST(pb_svarint_edge);
    RUN_TEST(pb_tag_roundtrip);
    RUN_TEST(pb_fixed32_roundtrip);
    RUN_TEST(pb_fixed64_roundtrip);
    RUN_TEST(pb_packed_svarint);
    RUN_TEST(pb_delta_decode);
    RUN_TEST(pb_skip_field_varint);
    RUN_TEST(pb_skip_field_fixed);

    printf("\nInflate/Deflate:\n");
    RUN_TEST(inflate_deflate_roundtrip);
    RUN_TEST(inflate_alloc);

    printf("\nString Table:\n");
    RUN_TEST(string_table_basic);
    RUN_TEST(string_table_large);

    printf("\nPBF Block Header:\n");
    RUN_TEST(block_header_init);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
