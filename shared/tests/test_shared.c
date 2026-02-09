/*
 * test_shared.c - Tests for shared library
 */

#include "shared.h"
#include "sh_circuit.h"
#include "sh_backoff.h"
#include "sh_retry.h"
#include "sh_cors.h"
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_completion.h"
#include "sh_worker_pool.h"
#include "sh_hashmap.h"
#include "sh_heap.h"
#include "sh_spatial_grid.h"
#include "sh_query.h"
#include "sh_render.h"
#include "sh_units.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

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
 * Arena Allocator Tests
 * ============================================================================ */

TEST(arena_create_free)
{
    SHArena *arena = sh_arena_create(1024);
    ASSERT(arena != NULL);
    ASSERT_EQ(sh_arena_remaining(arena), 1024);
    ASSERT_EQ(sh_arena_used(arena), 0);
    sh_arena_free(arena);
}

TEST(arena_alloc_basic)
{
    SHArena *arena = sh_arena_create(1024);
    ASSERT(arena != NULL);

    void *p1 = sh_arena_alloc(arena, 100);
    ASSERT(p1 != NULL);
    ASSERT(sh_arena_used(arena) >= 100);  /* May be aligned */

    void *p2 = sh_arena_alloc(arena, 200);
    ASSERT(p2 != NULL);
    ASSERT(p2 != p1);

    sh_arena_free(arena);
}

TEST(arena_calloc_zeroed)
{
    SHArena *arena = sh_arena_create(1024);
    int *arr = sh_arena_calloc(arena, 10, sizeof(int));
    ASSERT(arr != NULL);

    /* Verify zero-initialized */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(arr[i], 0);
    }
    sh_arena_free(arena);
}

TEST(arena_reset)
{
    SHArena *arena = sh_arena_create(1024);

    sh_arena_alloc(arena, 500);
    ASSERT(sh_arena_used(arena) >= 500);

    sh_arena_reset(arena);
    ASSERT_EQ(sh_arena_used(arena), 0);
    ASSERT_EQ(sh_arena_remaining(arena), 1024);

    /* Can allocate again after reset */
    void *p = sh_arena_alloc(arena, 100);
    ASSERT(p != NULL);

    sh_arena_free(arena);
}

TEST(arena_overflow_returns_null)
{
    SHArena *arena = sh_arena_create(100);

    void *p1 = sh_arena_alloc(arena, 50);
    ASSERT(p1 != NULL);

    /* This should fail - not enough space (accounting for alignment) */
    void *p2 = sh_arena_alloc(arena, 100);
    ASSERT(p2 == NULL);

    sh_arena_free(arena);
}

TEST(arena_alignment)
{
    SHArena *arena = sh_arena_create(1024);

    /* Allocate odd size, next alloc should still be aligned */
    sh_arena_alloc(arena, 1);
    void *p = sh_arena_alloc(arena, 8);

    /* Check 8-byte alignment */
    ASSERT(((uintptr_t)p % 8) == 0);

    sh_arena_free(arena);
}

/* ============================================================================
 * Memory Pool Tests
 * ============================================================================ */

TEST(pool_init_free)
{
    SHPool pool;
    int result = sh_pool_init(&pool, sizeof(int), 100);
    ASSERT_EQ(result, 0);
    ASSERT_EQ(sh_pool_remaining(&pool), 100);
    ASSERT_EQ(sh_pool_used(&pool), 0);
    sh_pool_free(&pool);
}

TEST(pool_alloc_basic)
{
    SHPool pool;
    sh_pool_init(&pool, sizeof(double), 100);

    size_t offset = sh_pool_alloc(&pool, 10);
    ASSERT(offset != SH_POOL_INVALID);
    ASSERT_EQ(offset, 0);
    ASSERT_EQ(sh_pool_used(&pool), 10);

    size_t offset2 = sh_pool_alloc(&pool, 5);
    ASSERT(offset2 != SH_POOL_INVALID);
    ASSERT_EQ(offset2, 10);

    sh_pool_free(&pool);
}

TEST(pool_ptr_access)
{
    SHPool pool;
    sh_pool_init(&pool, sizeof(int), 100);

    size_t offset = sh_pool_alloc(&pool, 5);
    int *arr = SH_POOL_PTR(&pool, int, offset);
    ASSERT(arr != NULL);

    /* Write and read back */
    arr[0] = 42;
    arr[4] = 99;
    ASSERT_EQ(SH_POOL_AT(&pool, int, offset, 0), 42);
    ASSERT_EQ(SH_POOL_AT(&pool, int, offset, 4), 99);

    sh_pool_free(&pool);
}

TEST(pool_reset)
{
    SHPool pool;
    sh_pool_init(&pool, sizeof(int), 100);

    sh_pool_alloc(&pool, 50);
    ASSERT_EQ(sh_pool_used(&pool), 50);

    sh_pool_reset(&pool);
    ASSERT_EQ(sh_pool_used(&pool), 0);
    ASSERT_EQ(sh_pool_remaining(&pool), 100);

    sh_pool_free(&pool);
}

TEST(pool_overflow_returns_invalid)
{
    SHPool pool;
    sh_pool_init(&pool, sizeof(int), 10);

    size_t offset = sh_pool_alloc(&pool, 5);
    ASSERT(offset != SH_POOL_INVALID);

    /* Should fail - not enough space */
    size_t offset2 = sh_pool_alloc(&pool, 10);
    ASSERT_EQ(offset2, SH_POOL_INVALID);

    sh_pool_free(&pool);
}

TEST(pool_grow)
{
    SHPool pool;
    sh_pool_init(&pool, sizeof(int), 10);

    sh_pool_alloc(&pool, 8);

    /* Grow pool */
    int result = sh_pool_grow(&pool, 2.0);
    ASSERT_EQ(result, 0);
    ASSERT(sh_pool_remaining(&pool) >= 10);  /* At least 10 more available */

    sh_pool_free(&pool);
}

/* ============================================================================
 * Rate Limiter Tests
 * ============================================================================ */

TEST(ratelimit_create_free)
{
    ShRateLimiter *limiter = sh_ratelimit_create(10.0, 100.0, 64);
    ASSERT(limiter != NULL);
    sh_ratelimit_free(limiter);
}

TEST(ratelimit_create_invalid_params)
{
    /* Zero/negative RPS should fail */
    ASSERT(sh_ratelimit_create(0.0, 100.0, 64) == NULL);
    ASSERT(sh_ratelimit_create(-1.0, 100.0, 64) == NULL);

    /* Zero/negative burst should fail */
    ASSERT(sh_ratelimit_create(10.0, 0.0, 64) == NULL);
    ASSERT(sh_ratelimit_create(10.0, -1.0, 64) == NULL);
}

TEST(ratelimit_free_null_safe)
{
    /* Should not crash */
    sh_ratelimit_free(NULL);
}

TEST(ratelimit_check_allows_within_burst)
{
    /* 10 RPS, burst of 5 - should allow 5 immediate requests */
    ShRateLimiter *limiter = sh_ratelimit_create(10.0, 5.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0x01020304);  /* 1.2.3.4 */

    /* First 5 requests should be allowed */
    for (int i = 0; i < 5; i++) {
        ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    }

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_check_denies_over_burst)
{
    /* Very low RPS (0.001) so refill is negligible during test, burst of 3 */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 3.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0x01020304);

    /* First 3 requests allowed (uses up the burst) */
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);

    /* 4th request denied (no tokens left, refill is negligible) */
    ASSERT(sh_ratelimit_check(limiter, &addr) == 0);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_different_ips_independent)
{
    /* Each IP gets its own bucket - use low RPS to avoid refill during test */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 2.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr1, addr2;
    sh_ratelimit_addr_ipv4(&addr1, 0x01020304);  /* 1.2.3.4 */
    sh_ratelimit_addr_ipv4(&addr2, 0x05060708);  /* 5.6.7.8 */

    /* Exhaust addr1's bucket */
    ASSERT(sh_ratelimit_check(limiter, &addr1) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr1) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr1) == 0);

    /* addr2 should still have full bucket */
    ASSERT(sh_ratelimit_check(limiter, &addr2) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr2) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr2) == 0);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_ipv6_support)
{
    /* Use low RPS to avoid refill during test */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 3.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv6(&addr, 0x20010db800000000ULL, 0x0000000000000001ULL);

    /* Should work the same as IPv4 */
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 0);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_ipv4_ipv6_different_buckets)
{
    /* IPv4 and IPv6 addresses should have separate buckets - low RPS */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 2.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr4, addr6;
    sh_ratelimit_addr_ipv4(&addr4, 0x01020304);
    sh_ratelimit_addr_ipv6(&addr6, 0x0000000001020304ULL, 0);  /* Same bits as IPv4 but different type */

    /* Exhaust IPv4 bucket */
    ASSERT(sh_ratelimit_check(limiter, &addr4) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr4) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr4) == 0);

    /* IPv6 should still have full bucket */
    ASSERT(sh_ratelimit_check(limiter, &addr6) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr6) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr6) == 0);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_stats)
{
    /* Low RPS to avoid refill during test */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 2.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0x01020304);

    /* Make some requests */
    sh_ratelimit_check(limiter, &addr);  /* allowed */
    sh_ratelimit_check(limiter, &addr);  /* allowed */
    sh_ratelimit_check(limiter, &addr);  /* denied */
    sh_ratelimit_check(limiter, &addr);  /* denied */

    ShRateLimitStats stats;
    sh_ratelimit_stats(limiter, &stats);

    ASSERT_EQ(stats.requests_allowed, 2);
    ASSERT_EQ(stats.requests_denied, 2);
    ASSERT_EQ(stats.active_entries, 1);
    ASSERT_EQ(stats.table_capacity, 64);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_reset)
{
    /* Low RPS to avoid refill during test */
    ShRateLimiter *limiter = sh_ratelimit_create(0.001, 2.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0x01020304);

    /* Exhaust bucket */
    sh_ratelimit_check(limiter, &addr);
    sh_ratelimit_check(limiter, &addr);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 0);

    /* Reset clears all entries */
    sh_ratelimit_reset(limiter);

    /* Should have full bucket again */
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    ASSERT(sh_ratelimit_check(limiter, &addr) == 0);

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_zero_addr_allowed)
{
    /* Zero address (0.0.0.0) should always be allowed - it's invalid */
    ShRateLimiter *limiter = sh_ratelimit_create(10.0, 1.0, 64);
    ASSERT(limiter != NULL);

    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0);

    /* Even with burst of 1, should always be allowed */
    for (int i = 0; i < 10; i++) {
        ASSERT(sh_ratelimit_check(limiter, &addr) == 1);
    }

    sh_ratelimit_free(limiter);
}

TEST(ratelimit_null_safe)
{
    ShRateLimitAddr addr;
    sh_ratelimit_addr_ipv4(&addr, 0x01020304);

    /* Should not crash, return denied (security: deny-by-default for invalid args) */
    ASSERT(sh_ratelimit_check(NULL, &addr) == 0);
    ASSERT(sh_ratelimit_check(NULL, NULL) == 0);

    /* Stats with NULL should not crash */
    sh_ratelimit_stats(NULL, NULL);
    sh_ratelimit_reset(NULL);
}

/* ============================================================================
 * Work Queue Tests
 * ============================================================================ */

TEST(workqueue_create_free)
{
    ShWorkQueue *queue = sh_workqueue_create(100, 5.0);
    ASSERT(queue != NULL);
    sh_workqueue_free(queue);
}

TEST(workqueue_create_invalid)
{
    /* Zero capacity should fail */
    ASSERT(sh_workqueue_create(0, 5.0) == NULL);
}

TEST(workqueue_free_null_safe)
{
    /* Should not crash */
    sh_workqueue_free(NULL);
}

TEST(workqueue_push_pop_basic)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 0);
    ASSERT(queue != NULL);

    /* Push an item */
    char *data = malloc(5);
    memcpy(data, "test", 5);
    ShWorkItem item = { .data = data, .data_len = 5, .user_ctx = (void*)0x1234 };

    ASSERT(sh_workqueue_push(queue, &item) == 1);
    ASSERT(sh_workqueue_depth(queue) == 1);

    /* Pop it */
    ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
    ASSERT(popped != NULL);
    ASSERT(popped->data_len == 5);
    ASSERT(memcmp(popped->data, "test", 5) == 0);
    ASSERT(popped->user_ctx == (void*)0x1234);

    sh_workqueue_item_free(popped);
    ASSERT(sh_workqueue_depth(queue) == 0);

    sh_workqueue_free(queue);
}

TEST(workqueue_full_returns_zero)
{
    /* Queue with capacity 2 */
    ShWorkQueue *queue = sh_workqueue_create(2, 0);
    ASSERT(queue != NULL);

    /* Push 2 items - should succeed */
    char *d1 = malloc(1); d1[0] = 'a';
    char *d2 = malloc(1); d2[0] = 'b';
    char *d3 = malloc(1); d3[0] = 'c';

    ShWorkItem i1 = { .data = d1, .data_len = 1 };
    ShWorkItem i2 = { .data = d2, .data_len = 1 };
    ShWorkItem i3 = { .data = d3, .data_len = 1 };

    ASSERT(sh_workqueue_push(queue, &i1) == 1);
    ASSERT(sh_workqueue_push(queue, &i2) == 1);
    ASSERT(sh_workqueue_full(queue) == 1);

    /* Third push should fail - queue full */
    ASSERT(sh_workqueue_push(queue, &i3) == 0);
    free(d3);  /* We still own d3 since push failed */

    sh_workqueue_free(queue);
}

TEST(workqueue_item_expiration)
{
    /* Queue with 0.001 second timeout */
    ShWorkQueue *queue = sh_workqueue_create(10, 0.001);
    ASSERT(queue != NULL);

    char *data = malloc(4);
    memcpy(data, "old", 4);
    ShWorkItem item = { .data = data, .data_len = 4 };
    ASSERT(sh_workqueue_push(queue, &item) == 1);

    /* Wait for item to expire */
    struct timespec ts = { 0, 10000000 };  /* 10ms */
    nanosleep(&ts, NULL);

    /* Pop and check expiration */
    ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
    ASSERT(popped != NULL);
    ASSERT(sh_workqueue_item_expired(queue, popped) == 1);
    ASSERT(sh_workqueue_item_age(popped) > 0.001);

    sh_workqueue_item_free(popped);
    sh_workqueue_free(queue);
}

TEST(workqueue_stats)
{
    ShWorkQueue *queue = sh_workqueue_create(3, 1.0);
    ASSERT(queue != NULL);

    /* Push 2 items */
    char *d1 = malloc(1);
    char *d2 = malloc(1);
    ShWorkItem i1 = { .data = d1, .data_len = 1 };
    ShWorkItem i2 = { .data = d2, .data_len = 1 };

    sh_workqueue_push(queue, &i1);
    sh_workqueue_push(queue, &i2);

    ShWorkQueueStats stats;
    sh_workqueue_stats(queue, &stats);

    ASSERT_EQ(stats.current_depth, 2);
    ASSERT_EQ(stats.max_capacity, 3);
    ASSERT_EQ(stats.total_pushed, 2);
    ASSERT_EQ(stats.total_popped, 0);
    ASSERT_EQ(stats.total_dropped, 0);
    ASSERT_NEAR(stats.timeout_sec, 1.0, 0.001);

    /* Pop one */
    ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
    sh_workqueue_item_free(popped);

    sh_workqueue_stats(queue, &stats);
    ASSERT_EQ(stats.current_depth, 1);
    ASSERT_EQ(stats.total_popped, 1);

    sh_workqueue_free(queue);
}

TEST(workqueue_try_push_pressure)
{
    ShWorkQueue *queue = sh_workqueue_create(4, 0);
    ASSERT(queue != NULL);

    double pressure;

    char *d1 = malloc(1);
    ShWorkItem i1 = { .data = d1, .data_len = 1 };
    ASSERT(sh_workqueue_try_push(queue, &i1, &pressure) == 1);
    ASSERT_NEAR(pressure, 0.0, 0.01);  /* Was empty before push */

    char *d2 = malloc(1);
    ShWorkItem i2 = { .data = d2, .data_len = 1 };
    sh_workqueue_try_push(queue, &i2, &pressure);
    ASSERT_NEAR(pressure, 0.25, 0.01);  /* 1/4 */

    char *d3 = malloc(1);
    ShWorkItem i3 = { .data = d3, .data_len = 1 };
    sh_workqueue_try_push(queue, &i3, &pressure);
    ASSERT_NEAR(pressure, 0.5, 0.01);  /* 2/4 */

    sh_workqueue_free(queue);
}

TEST(workqueue_shutdown)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 0);
    ASSERT(queue != NULL);

    /* Shutdown empty queue */
    sh_workqueue_shutdown(queue);

    /* Pop should return NULL immediately */
    ASSERT(sh_workqueue_pop_timeout(queue, 0) == NULL);

    /* Push should fail after shutdown */
    char *data = malloc(1);
    ShWorkItem item = { .data = data, .data_len = 1 };
    ASSERT(sh_workqueue_push(queue, &item) == 0);
    free(data);

    sh_workqueue_free(queue);
}

TEST(workqueue_null_safety)
{
    ShWorkItem item = { .data = NULL, .data_len = 0 };

    /* All these should not crash */
    ASSERT(sh_workqueue_push(NULL, &item) == 0);
    ASSERT(sh_workqueue_push(NULL, NULL) == 0);
    ASSERT(sh_workqueue_pop(NULL) == NULL);
    ASSERT(sh_workqueue_pop_timeout(NULL, 0) == NULL);
    ASSERT(sh_workqueue_item_expired(NULL, NULL) == 0);
    ASSERT_NEAR(sh_workqueue_item_age(NULL), 0.0, 0.001);
    ASSERT(sh_workqueue_depth(NULL) == 0);
    ASSERT(sh_workqueue_full(NULL) == 1);
    sh_workqueue_item_free(NULL);
    sh_workqueue_shutdown(NULL);
}

TEST(workqueue_fifo_order)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 0);
    ASSERT(queue != NULL);

    /* Push 3 items with distinct data */
    for (int i = 0; i < 3; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        ShWorkItem item = { .data = data, .data_len = sizeof(int) };
        sh_workqueue_push(queue, &item);
    }

    /* Pop should return in FIFO order */
    for (int i = 0; i < 3; i++) {
        ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
        ASSERT(popped != NULL);
        ASSERT_EQ(*(int*)popped->data, i);
        sh_workqueue_item_free(popped);
    }

    sh_workqueue_free(queue);
}

TEST(workqueue_item_cancel_basic)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 0);
    ASSERT(queue != NULL);

    /* Push an item */
    int *data = malloc(sizeof(int));
    *data = 42;
    ShWorkItem item = { .data = data, .data_len = sizeof(int) };
    ASSERT(sh_workqueue_push(queue, &item) == 1);

    /* Pop and verify not cancelled initially */
    ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
    ASSERT(popped != NULL);
    ASSERT(sh_workqueue_item_cancelled(popped) == 0);

    /* Cancel the item */
    sh_workqueue_item_cancel(queue, popped);
    ASSERT(sh_workqueue_item_cancelled(popped) == 1);

    /* Verify stats updated */
    ShWorkQueueStats stats;
    sh_workqueue_stats(queue, &stats);
    ASSERT_EQ(stats.total_cancelled, (uint64_t)1);

    sh_workqueue_item_free(popped);
    sh_workqueue_free(queue);
}

TEST(workqueue_item_cancel_null_queue)
{
    /* Cancel should work without queue (just won't update stats) */
    int *data = malloc(sizeof(int));
    *data = 42;
    ShWorkItem item = { .data = data, .data_len = sizeof(int), .cancelled = 0 };

    /* Cancel without queue - should still set flag */
    sh_workqueue_item_cancel(NULL, &item);
    ASSERT(sh_workqueue_item_cancelled(&item) == 1);

    free(data);
}

TEST(workqueue_item_cancel_null_safety)
{
    /* Should handle NULL gracefully */
    sh_workqueue_item_cancel(NULL, NULL);
    ASSERT(sh_workqueue_item_cancelled(NULL) == 0);
}

TEST(workqueue_item_cancel_multiple_times)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 0);
    ASSERT(queue != NULL);

    int *data = malloc(sizeof(int));
    *data = 1;
    ShWorkItem item = { .data = data, .data_len = sizeof(int) };
    sh_workqueue_push(queue, &item);

    ShWorkItem *popped = sh_workqueue_pop_timeout(queue, 0);
    ASSERT(popped != NULL);

    /* Cancel multiple times - should only increment stats once per call */
    sh_workqueue_item_cancel(queue, popped);
    sh_workqueue_item_cancel(queue, popped);
    sh_workqueue_item_cancel(queue, popped);

    /* Item should still be cancelled */
    ASSERT(sh_workqueue_item_cancelled(popped) == 1);

    /* Stats should show 3 cancellations (each call increments) */
    ShWorkQueueStats stats;
    sh_workqueue_stats(queue, &stats);
    ASSERT_EQ(stats.total_cancelled, (uint64_t)3);

    sh_workqueue_item_free(popped);
    sh_workqueue_free(queue);
}

/* ============================================================================
 * Completion Signaling Tests
 * ============================================================================ */

TEST(completion_init_cleanup)
{
    ShCompletion comp;
    sh_completion_init(&comp);
    ASSERT_EQ(comp.completed, 0);
    ASSERT_EQ(comp.cancelled, 0);
    sh_completion_cleanup(&comp);
}

TEST(completion_init_null_safe)
{
    /* Should not crash */
    sh_completion_init(NULL);
    sh_completion_cleanup(NULL);
}

TEST(completion_signal_immediate)
{
    ShCompletion comp;
    sh_completion_init(&comp);

    /* Signal completion before wait */
    sh_completion_signal(&comp);
    ASSERT_EQ(comp.completed, 1);

    /* Wait should return immediately */
    int result = sh_completion_wait(&comp, 1000);
    ASSERT_EQ(result, 1);

    sh_completion_cleanup(&comp);
}

TEST(completion_cancel)
{
    ShCompletion comp;
    sh_completion_init(&comp);

    ASSERT_EQ(sh_completion_is_cancelled(&comp), 0);
    sh_completion_cancel(&comp);
    ASSERT_EQ(sh_completion_is_cancelled(&comp), 1);

    sh_completion_cleanup(&comp);
}

TEST(completion_cancel_null_safe)
{
    /* Should not crash */
    sh_completion_cancel(NULL);
    ASSERT_EQ(sh_completion_is_cancelled(NULL), 0);
}

TEST(completion_signal_null_safe)
{
    /* Should not crash */
    sh_completion_signal(NULL);
}

/* Worker thread for completion tests */
static void *completion_worker_thread(void *arg)
{
    ShCompletion *comp = (ShCompletion *)arg;

    /* Simulate some work */
    usleep(50000);  /* 50ms */

    /* Signal completion */
    sh_completion_signal(comp);
    return NULL;
}

TEST(completion_wait_success)
{
    ShCompletion comp;
    sh_completion_init(&comp);

    /* Start worker thread */
    pthread_t worker;
    pthread_create(&worker, NULL, completion_worker_thread, &comp);

    /* Wait for completion with generous timeout */
    int result = sh_completion_wait(&comp, 1000);
    ASSERT_EQ(result, 1);  /* Should complete */

    pthread_join(worker, NULL);
    sh_completion_cleanup(&comp);
}

TEST(completion_wait_timeout)
{
    ShCompletion comp;
    sh_completion_init(&comp);

    /* Wait with short timeout - should fail (no one signals) */
    int result = sh_completion_wait(&comp, 50);
    ASSERT_EQ(result, 0);  /* Timeout */

    sh_completion_cleanup(&comp);
}

/* ============================================================================
 * Worker Pool Tests
 * ============================================================================ */

/* Counter for worker pool tests */
static volatile int s_pool_items_processed = 0;
static pthread_mutex_t s_pool_test_mutex = PTHREAD_MUTEX_INITIALIZER;

static void pool_test_callback(ShWorkItem *item, void *ctx)
{
    (void)ctx;
    if (!item) return;

    /* Simulate some work */
    usleep(10000);  /* 10ms */

    /* Increment counter */
    pthread_mutex_lock(&s_pool_test_mutex);
    s_pool_items_processed++;
    pthread_mutex_unlock(&s_pool_test_mutex);

    sh_workqueue_item_free(item);
}

TEST(worker_pool_create_free)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 5.0);
    ASSERT(queue != NULL);

    ShWorkerPoolConfig cfg = {
        .queue = queue,
        .callback = pool_test_callback,
        .ctx = NULL
    };

    ShWorkerPool *pool = sh_worker_pool_create(2, &cfg);
    ASSERT(pool != NULL);
    ASSERT(sh_worker_pool_size(pool) == 2);
    ASSERT(sh_worker_pool_queue(pool) == queue);

    sh_worker_pool_stop(pool);
    sh_worker_pool_join(pool);
    sh_worker_pool_free(pool);
    sh_workqueue_free(queue);
}

TEST(worker_pool_create_invalid)
{
    ShWorkerPoolConfig cfg = { .queue = NULL, .callback = NULL };

    /* NULL config */
    ASSERT(sh_worker_pool_create(2, NULL) == NULL);

    /* NULL queue */
    cfg.callback = pool_test_callback;
    ASSERT(sh_worker_pool_create(2, &cfg) == NULL);

    /* NULL callback */
    ShWorkQueue *queue = sh_workqueue_create(10, 5.0);
    cfg.queue = queue;
    cfg.callback = NULL;
    ASSERT(sh_worker_pool_create(2, &cfg) == NULL);

    sh_workqueue_free(queue);
}

TEST(worker_pool_null_safe)
{
    /* These should not crash */
    sh_worker_pool_stop(NULL);
    sh_worker_pool_join(NULL);
    sh_worker_pool_free(NULL);
    ASSERT(sh_worker_pool_size(NULL) == 0);
    ASSERT(sh_worker_pool_queue(NULL) == NULL);
}

TEST(worker_pool_auto_detect)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 5.0);
    ASSERT(queue != NULL);

    ShWorkerPoolConfig cfg = {
        .queue = queue,
        .callback = pool_test_callback
    };

    /* 0 = auto-detect CPU count */
    ShWorkerPool *pool = sh_worker_pool_create(0, &cfg);
    ASSERT(pool != NULL);
    ASSERT(sh_worker_pool_size(pool) >= 1);  /* At least 1 worker */

    sh_worker_pool_stop(pool);
    sh_worker_pool_join(pool);
    sh_worker_pool_free(pool);
    sh_workqueue_free(queue);
}

TEST(worker_pool_processes_items)
{
    ShWorkQueue *queue = sh_workqueue_create(10, 5.0);
    ASSERT(queue != NULL);

    s_pool_items_processed = 0;

    ShWorkerPoolConfig cfg = {
        .queue = queue,
        .callback = pool_test_callback
    };

    ShWorkerPool *pool = sh_worker_pool_create(2, &cfg);
    ASSERT(pool != NULL);

    /* Push 5 items */
    for (int i = 0; i < 5; i++) {
        int *data = malloc(sizeof(int));
        *data = i;
        ShWorkItem item = { .data = data, .data_len = sizeof(int) };
        ASSERT(sh_workqueue_push(queue, &item) == 1);
    }

    /* Wait for processing (with timeout) */
    for (int i = 0; i < 100 && s_pool_items_processed < 5; i++) {
        usleep(20000);  /* 20ms */
    }

    ASSERT_EQ(s_pool_items_processed, 5);

    sh_worker_pool_stop(pool);
    sh_worker_pool_join(pool);
    sh_worker_pool_free(pool);
    sh_workqueue_free(queue);
}

/* ============================================================================
 * Capacity Planning Tests
 * ============================================================================ */

TEST(capacity_calculate_basic)
{
    ShCapacityParams params;
    ShCapacityInput input = {
        .avg_response_ms = 75.0,      /* 75ms average response */
        .p99_response_ms = 0,         /* Auto-estimate */
        .num_workers = 8,             /* 8 worker threads */
        .target_utilization = 0.7,    /* 70% target */
        .client_timeout_ms = 10000,   /* 10s client timeout */
        .burst_tiles = 25,            /* 25 tiles in initial view */
        .expected_clients = 10        /* 10 concurrent clients */
    };

    int result = sh_capacity_calculate(&params, &input);
    ASSERT_EQ(result, 1);

    /* Service rate: 1000/75 = 13.3 RPS per worker */
    /* Max throughput: 8 * 13.3 * 0.7 = 74.7 RPS */
    ASSERT(params.max_throughput_rps > 70.0);
    ASSERT(params.max_throughput_rps < 80.0);

    /* Rate limit per IP should be max_throughput / expected_clients */
    ASSERT(params.rate_limit_rps > 5.0);
    ASSERT(params.rate_limit_rps < 15.0);

    /* Burst should accommodate initial tile load */
    ASSERT(params.rate_limit_burst >= 25.0);

    /* Queue depth should be reasonable */
    ASSERT(params.queue_depth >= 8);       /* At least worker count */
    ASSERT(params.queue_depth <= 10000);   /* Cap at 10k */

    /* Timeout should be less than client timeout */
    ASSERT(params.queue_timeout_sec > 0);
    ASSERT(params.queue_timeout_sec < 10.0);
}

TEST(capacity_calculate_invalid_input)
{
    ShCapacityParams params;
    ShCapacityInput input = {0};

    /* NULL parameters */
    ASSERT_EQ(sh_capacity_calculate(NULL, &input), 0);
    ASSERT_EQ(sh_capacity_calculate(&params, NULL), 0);

    /* Zero response time */
    input.avg_response_ms = 0;
    input.num_workers = 8;
    input.target_utilization = 0.7;
    ASSERT_EQ(sh_capacity_calculate(&params, &input), 0);

    /* Zero workers */
    input.avg_response_ms = 75.0;
    input.num_workers = 0;
    ASSERT_EQ(sh_capacity_calculate(&params, &input), 0);

    /* Invalid utilization */
    input.num_workers = 8;
    input.target_utilization = 0;
    ASSERT_EQ(sh_capacity_calculate(&params, &input), 0);

    input.target_utilization = 1.5;
    ASSERT_EQ(sh_capacity_calculate(&params, &input), 0);
}

TEST(capacity_calculate_defaults)
{
    ShCapacityParams params;
    ShCapacityInput input = {
        .avg_response_ms = 100.0,
        .num_workers = 4,
        .target_utilization = 0.8,
        /* All other fields default to 0 */
    };

    int result = sh_capacity_calculate(&params, &input);
    ASSERT_EQ(result, 1);

    /* Should use defaults for missing values */
    ASSERT(params.rate_limit_rps > 0);
    ASSERT(params.rate_limit_burst > 0);
    ASSERT(params.queue_depth > 0);
    ASSERT(params.queue_timeout_sec > 0);
}

TEST(capacity_calculate_high_load)
{
    ShCapacityParams params;
    ShCapacityInput input = {
        .avg_response_ms = 200.0,     /* Slow responses */
        .num_workers = 2,             /* Few workers */
        .target_utilization = 0.9,    /* High utilization */
        .client_timeout_ms = 5000,    /* Short timeout */
        .burst_tiles = 50,            /* Large burst */
        .expected_clients = 5
    };

    int result = sh_capacity_calculate(&params, &input);
    ASSERT_EQ(result, 1);

    /* With slow responses, throughput is lower */
    /* Service rate: 1000/200 = 5 RPS per worker */
    /* Max throughput: 2 * 5 * 0.9 = 9 RPS */
    ASSERT(params.max_throughput_rps > 5.0);
    ASSERT(params.max_throughput_rps < 15.0);

    /* Burst should handle burst_tiles */
    ASSERT(params.rate_limit_burst >= 50.0);
}

TEST(capacity_report)
{
    ShCapacityParams params = {
        .rate_limit_rps = 7.5,
        .rate_limit_burst = 50.0,
        .queue_depth = 100,
        .queue_timeout_sec = 9.5,
        .max_throughput_rps = 75.0,
        .expected_wait_ms = 5.0,
        .headroom_factor = 1.43
    };

    char buf[1024];
    int len = sh_capacity_report(&params, buf, sizeof(buf));

    ASSERT(len > 0);
    ASSERT(strstr(buf, "7.5") != NULL);    /* Rate limit */
    ASSERT(strstr(buf, "50") != NULL);     /* Burst */
    ASSERT(strstr(buf, "100") != NULL);    /* Queue depth */
    ASSERT(strstr(buf, "9.5") != NULL);    /* Timeout */
}

TEST(capacity_report_null_safe)
{
    ShCapacityParams params = {0};
    char buf[100];

    ASSERT_EQ(sh_capacity_report(NULL, buf, sizeof(buf)), 0);
    ASSERT_EQ(sh_capacity_report(&params, NULL, sizeof(buf)), 0);
    ASSERT_EQ(sh_capacity_report(&params, buf, 0), 0);
}

TEST(capacity_validate_good_config)
{
    char warnings[512];

    /* Configuration that matches performance */
    /* Service rate: 1000/75 = 13.3 RPS/worker */
    /* Max throughput: 8 * 13.3 = 106.7 RPS */
    /* Rate limit should be between 10% (10.67) and 80% (85.3) */
    int count = sh_capacity_validate(
        100,        /* queue_depth */
        9.0,        /* timeout_sec */
        20.0,       /* rate_limit_rps - within acceptable range */
        75.0,       /* measured_response_ms */
        8,          /* num_workers */
        warnings,
        sizeof(warnings)
    );

    /* Should have no warnings for well-configured system */
    ASSERT_EQ(count, 0);
}

TEST(capacity_validate_queue_too_deep)
{
    char warnings[512];

    /* Queue so deep it can't drain in time */
    int count = sh_capacity_validate(
        1000,       /* queue_depth - way too deep */
        5.0,        /* timeout_sec - short */
        10.0,       /* rate_limit_rps */
        100.0,      /* measured_response_ms */
        4,          /* num_workers */
        warnings,
        sizeof(warnings)
    );

    /* Should warn about queue being too deep */
    ASSERT(count > 0);
    ASSERT(strstr(warnings, "Queue too deep") != NULL);
}

TEST(capacity_validate_timeout_too_short)
{
    char warnings[512];

    /* Timeout shorter than 2x response time */
    int count = sh_capacity_validate(
        10,         /* queue_depth */
        0.1,        /* timeout_sec - way too short */
        10.0,       /* rate_limit_rps */
        100.0,      /* measured_response_ms = 0.1s */
        4,          /* num_workers */
        warnings,
        sizeof(warnings)
    );

    ASSERT(count > 0);
    ASSERT(strstr(warnings, "Timeout too short") != NULL);
}

TEST(capacity_validate_null_safe)
{
    char warnings[512];

    /* NULL warnings buffer */
    ASSERT_EQ(sh_capacity_validate(100, 9.0, 10.0, 75.0, 8, NULL, 512), 0);

    /* Zero buffer size */
    ASSERT_EQ(sh_capacity_validate(100, 9.0, 10.0, 75.0, 8, warnings, 0), 0);
}

/* ============================================================================
 * Adaptive Capacity Tests
 * ============================================================================ */

TEST(adaptive_create_free)
{
    ShAdaptiveConfig config;
    sh_adaptive_config_init(&config);
    config.num_workers = 4;

    ShAdaptiveTracker *tracker = sh_adaptive_create(&config);
    ASSERT(tracker != NULL);

    sh_adaptive_free(tracker);
}

TEST(adaptive_create_defaults)
{
    /* NULL config uses defaults */
    ShAdaptiveTracker *tracker = sh_adaptive_create(NULL);
    ASSERT(tracker != NULL);
    sh_adaptive_free(tracker);
}

TEST(adaptive_free_null_safe)
{
    sh_adaptive_free(NULL);  /* Should not crash */
}

TEST(adaptive_record_basic)
{
    ShAdaptiveTracker *tracker = sh_adaptive_create(NULL);
    ASSERT(tracker != NULL);

    /* Record some samples */
    sh_adaptive_record(tracker, 50.0);
    sh_adaptive_record(tracker, 75.0);
    sh_adaptive_record(tracker, 100.0);

    ShAdaptiveStats stats;
    sh_adaptive_stats(tracker, &stats);

    ASSERT_EQ(stats.sample_count, 3);
    ASSERT_NEAR(stats.avg_ms, 75.0, 0.1);
    ASSERT_NEAR(stats.min_ms, 50.0, 0.1);
    ASSERT_NEAR(stats.max_ms, 100.0, 0.1);

    sh_adaptive_free(tracker);
}

TEST(adaptive_percentiles)
{
    ShAdaptiveConfig config;
    sh_adaptive_config_init(&config);
    config.window_size = 100;

    ShAdaptiveTracker *tracker = sh_adaptive_create(&config);
    ASSERT(tracker != NULL);

    /* Record 100 samples: 1, 2, 3, ..., 100 */
    for (int i = 1; i <= 100; i++) {
        sh_adaptive_record(tracker, (double)i);
    }

    ShAdaptiveStats stats;
    sh_adaptive_stats(tracker, &stats);

    /* P50 should be around 50 */
    ASSERT(stats.p50_ms > 45 && stats.p50_ms < 55);

    /* P99 should be around 99 */
    ASSERT(stats.p99_ms > 95 && stats.p99_ms <= 100);

    sh_adaptive_free(tracker);
}

TEST(adaptive_recalculate)
{
    ShAdaptiveConfig config;
    sh_adaptive_config_init(&config);
    config.num_workers = 8;
    config.target_utilization = 0.7;
    config.window_size = 100;

    ShAdaptiveTracker *tracker = sh_adaptive_create(&config);
    ASSERT(tracker != NULL);

    /* Record samples around 75ms */
    for (int i = 0; i < 100; i++) {
        sh_adaptive_record(tracker, 70.0 + (double)(i % 10));
    }

    ShCapacityParams params;
    int result = sh_adaptive_recalculate(tracker, &params);
    ASSERT_EQ(result, 1);

    /* Should have calculated reasonable params */
    ASSERT(params.rate_limit_rps > 0);
    ASSERT(params.queue_depth > 0);
    ASSERT(params.max_throughput_rps > 0);

    sh_adaptive_free(tracker);
}

TEST(adaptive_update_interval)
{
    ShAdaptiveConfig config;
    sh_adaptive_config_init(&config);
    config.recalc_interval = 10;  /* Recalc every 10 samples */
    config.window_size = 100;

    ShAdaptiveTracker *tracker = sh_adaptive_create(&config);
    ASSERT(tracker != NULL);

    ShCapacityParams params;

    /* First 9 samples should not trigger recalc */
    for (int i = 0; i < 9; i++) {
        sh_adaptive_record(tracker, 50.0);
        ASSERT_EQ(sh_adaptive_update(tracker, &params), 0);
    }

    /* 10th sample should trigger recalc */
    sh_adaptive_record(tracker, 50.0);
    ASSERT_EQ(sh_adaptive_update(tracker, &params), 1);

    sh_adaptive_free(tracker);
}

TEST(adaptive_sample_cap)
{
    ShAdaptiveConfig config;
    sh_adaptive_config_init(&config);
    config.max_sample_ms = 1000.0;  /* Cap at 1s */

    ShAdaptiveTracker *tracker = sh_adaptive_create(&config);
    ASSERT(tracker != NULL);

    /* Record a very large sample */
    sh_adaptive_record(tracker, 99999.0);

    ShAdaptiveStats stats;
    sh_adaptive_stats(tracker, &stats);

    /* Should be capped at 1000ms */
    ASSERT_NEAR(stats.max_ms, 1000.0, 0.1);

    sh_adaptive_free(tracker);
}

TEST(adaptive_get_params)
{
    ShAdaptiveTracker *tracker = sh_adaptive_create(NULL);
    ASSERT(tracker != NULL);

    ShCapacityParams params;

    /* No params before first calculation */
    ASSERT_EQ(sh_adaptive_get_params(tracker, &params), 0);

    /* Record and recalculate */
    sh_adaptive_record(tracker, 50.0);
    sh_adaptive_recalculate(tracker, NULL);

    /* Now params should be available */
    ASSERT_EQ(sh_adaptive_get_params(tracker, &params), 1);
    ASSERT(params.rate_limit_rps > 0);

    sh_adaptive_free(tracker);
}

TEST(adaptive_null_safety)
{
    ShCapacityParams params;
    ShAdaptiveStats stats;

    /* All should not crash with NULL */
    sh_adaptive_record(NULL, 50.0);
    ASSERT_EQ(sh_adaptive_update(NULL, &params), 0);
    ASSERT_EQ(sh_adaptive_recalculate(NULL, &params), 0);
    sh_adaptive_stats(NULL, &stats);
    sh_adaptive_set_callback(NULL, NULL, NULL);
    ASSERT_EQ(sh_adaptive_get_params(NULL, &params), 0);
}

/* ============================================================================
 * Args Parsing Tests
 * ============================================================================ */

TEST(args_init)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    /* Check defaults */
    ASSERT_EQ(cfg.port, 8080);
    ASSERT(strcmp(cfg.host, "0.0.0.0") == 0);
    ASSERT_EQ(cfg.rate_limit_enabled, 1);
    ASSERT_NEAR(cfg.rate_limit_rps, 10.0, 0.1);
    ASSERT_EQ(cfg.work_queue_enabled, 1);
    ASSERT_EQ(cfg.adaptive_enabled, 0);
}

TEST(args_parse_basic)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    char *argv[] = {"prog", "-p", "9000", "-t", "4", "data.pbf"};
    int argc = 6;

    int idx = sh_args_parse(&cfg, argc, argv);

    ASSERT_EQ(idx, 5);  /* Index of data.pbf */
    ASSERT_EQ(cfg.port, 9000);
    ASSERT_EQ(cfg.worker_threads, 4);
}

TEST(args_parse_rate_limit)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    char *argv[] = {"prog", "--rate-limit-rps", "20", "--rate-limit-burst", "200"};
    int argc = 5;

    sh_args_parse(&cfg, argc, argv);

    ASSERT_NEAR(cfg.rate_limit_rps, 20.0, 0.1);
    ASSERT_NEAR(cfg.rate_limit_burst, 200.0, 0.1);
}

TEST(args_parse_rate_limit_off)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    char *argv[] = {"prog", "--rate-limit-off"};
    int argc = 2;

    sh_args_parse(&cfg, argc, argv);

    ASSERT_EQ(cfg.rate_limit_enabled, 0);
}

TEST(args_parse_adaptive)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    char *argv[] = {"prog", "--adaptive", "--utilization", "0.8"};
    int argc = 4;

    sh_args_parse(&cfg, argc, argv);

    ASSERT_EQ(cfg.adaptive_enabled, 1);
    ASSERT_NEAR(cfg.target_utilization, 0.8, 0.01);
}

TEST(args_parse_positional)
{
    ShServerConfig cfg;
    sh_args_init(&cfg);

    /* First non-option is the positional arg */
    char *argv[] = {"prog", "-p", "8081", "mydata.pbf", "extra"};
    int argc = 5;

    int idx = sh_args_parse(&cfg, argc, argv);

    ASSERT_EQ(idx, 3);  /* Index of mydata.pbf */
    ASSERT(strcmp(argv[idx], "mydata.pbf") == 0);
}

TEST(args_prefix)
{
    ASSERT(strcmp(sh_args_prefix(SH_API_CARTA), "CARTA") == 0);
    ASSERT(strcmp(sh_args_prefix(SH_API_VELO), "VELO") == 0);
    ASSERT(strcmp(sh_args_prefix(SH_API_LOCUS), "LOCUS") == 0);
    ASSERT(strcmp(sh_args_prefix(SH_API_FUELWISE), "FUELWISE") == 0);
}

/* ============================================================================
 * Circuit Breaker Tests
 * ============================================================================ */

TEST(circuit_create_free)
{
    ShCircuitBreaker *cb = sh_circuit_create(NULL);
    ASSERT(cb != NULL);
    sh_circuit_free(cb);
}

TEST(circuit_create_with_config)
{
    ShCircuitConfig config = {
        .failure_threshold = 3,
        .success_threshold = 1,
        .open_duration_ms = 5000.0
    };
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);
    sh_circuit_free(cb);
}

TEST(circuit_free_null_safe)
{
    sh_circuit_free(NULL);  /* Should not crash */
}

TEST(circuit_allow_closed)
{
    ShCircuitBreaker *cb = sh_circuit_create(NULL);
    ASSERT(cb != NULL);

    /* Closed circuit always allows */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(sh_circuit_allow(cb), 1);
    }

    sh_circuit_free(cb);
}

TEST(circuit_opens_on_failures)
{
    ShCircuitConfig config = { .failure_threshold = 3, .success_threshold = 2, .open_duration_ms = 100000.0 };
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);

    /* Record 3 failures - should trip circuit */
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    /* Now allow should return 0 */
    ASSERT_EQ(sh_circuit_allow(cb), 0);

    sh_circuit_free(cb);
}

TEST(circuit_success_resets_failures)
{
    ShCircuitConfig config = { .failure_threshold = 3, .success_threshold = 2, .open_duration_ms = 100000.0 };
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);

    /* 2 failures, then success - should reset */
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 1);  /* Success resets counter */
    sh_circuit_record(cb, 0);  /* This is now failure #1 */
    sh_circuit_record(cb, 0);  /* Failure #2 */

    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);

    sh_circuit_free(cb);
}

TEST(circuit_half_open_recovers)
{
    ShCircuitConfig config = { .failure_threshold = 2, .success_threshold = 2, .open_duration_ms = 1.0 }; /* 1ms */
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);

    /* Trip the circuit */
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    /* Wait for open duration */
    struct timespec ts = { 0, 5000000 };  /* 5ms */
    nanosleep(&ts, NULL);

    /* Allow should transition to half-open */
    ASSERT_EQ(sh_circuit_allow(cb), 1);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_HALF_OPEN);

    /* Record successes to close */
    sh_circuit_record(cb, 1);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_HALF_OPEN);
    sh_circuit_record(cb, 1);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);

    sh_circuit_free(cb);
}

TEST(circuit_half_open_failure_reopens)
{
    ShCircuitConfig config = { .failure_threshold = 2, .success_threshold = 2, .open_duration_ms = 1.0 };
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);

    /* Trip the circuit */
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    /* Wait and transition to half-open */
    struct timespec ts = { 0, 5000000 };
    nanosleep(&ts, NULL);
    sh_circuit_allow(cb);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_HALF_OPEN);

    /* Failure should reopen */
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    sh_circuit_free(cb);
}

TEST(circuit_stats)
{
    ShCircuitBreaker *cb = sh_circuit_create(NULL);
    ASSERT(cb != NULL);

    sh_circuit_allow(cb);
    sh_circuit_allow(cb);
    sh_circuit_record(cb, 1);
    sh_circuit_record(cb, 0);

    ShCircuitStats stats;
    sh_circuit_stats(cb, &stats);

    ASSERT_EQ(stats.total_requests, 2);
    ASSERT_EQ(stats.total_failures, 1);
    ASSERT_EQ(stats.state, SH_CIRCUIT_CLOSED);

    sh_circuit_free(cb);
}

TEST(circuit_reset)
{
    ShCircuitConfig config = { .failure_threshold = 2, .success_threshold = 2, .open_duration_ms = 100000.0 };
    ShCircuitBreaker *cb = sh_circuit_create(&config);
    ASSERT(cb != NULL);

    /* Trip the circuit */
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    /* Reset should return to closed */
    sh_circuit_reset(cb);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_CLOSED);
    ASSERT_EQ(sh_circuit_allow(cb), 1);

    sh_circuit_free(cb);
}

/* ============================================================================
 * Backoff Tests
 * ============================================================================ */

TEST(backoff_init)
{
    ShBackoff backoff;
    sh_backoff_init(&backoff, NULL);

    ASSERT_EQ(sh_backoff_attempt(&backoff), 0);
    ASSERT_EQ(sh_backoff_has_retries(&backoff), 1);
}

TEST(backoff_init_with_config)
{
    ShBackoffConfig config = { .base_delay_ms = 200.0, .max_delay_ms = 5000.0, .max_retries = 3, .jitter_factor = 0.0 };
    ShBackoff backoff;
    sh_backoff_init(&backoff, &config);

    ASSERT_EQ(sh_backoff_has_retries(&backoff), 1);
}

TEST(backoff_exponential)
{
    ShBackoffConfig config = { .base_delay_ms = 100.0, .max_delay_ms = 10000.0, .max_retries = 5, .jitter_factor = 0.0 };
    ShBackoff backoff;
    sh_backoff_init(&backoff, &config);

    /* Without jitter: 100, 200, 400, 800, 1600 */
    double d0 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d0, 100.0, 1.0);

    double d1 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d1, 200.0, 1.0);

    double d2 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d2, 400.0, 1.0);

    double d3 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d3, 800.0, 1.0);

    double d4 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d4, 1600.0, 1.0);

    /* No more retries */
    ASSERT_EQ(sh_backoff_has_retries(&backoff), 0);
}

TEST(backoff_max_cap)
{
    ShBackoffConfig config = { .base_delay_ms = 1000.0, .max_delay_ms = 2000.0, .max_retries = 5, .jitter_factor = 0.0 };
    ShBackoff backoff;
    sh_backoff_init(&backoff, &config);

    /* 1000, 2000 (capped), 2000, 2000, 2000 */
    sh_backoff_next(&backoff);  /* 1000 */
    double d1 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d1, 2000.0, 1.0);

    double d2 = sh_backoff_next(&backoff);
    ASSERT_NEAR(d2, 2000.0, 1.0);
}

TEST(backoff_reset)
{
    ShBackoffConfig config = { .base_delay_ms = 100.0, .max_delay_ms = 10000.0, .max_retries = 5, .jitter_factor = 0.0 };
    ShBackoff backoff;
    sh_backoff_init(&backoff, &config);

    sh_backoff_next(&backoff);
    sh_backoff_next(&backoff);
    ASSERT_EQ(sh_backoff_attempt(&backoff), 2);

    sh_backoff_reset(&backoff);
    ASSERT_EQ(sh_backoff_attempt(&backoff), 0);

    double d = sh_backoff_next(&backoff);
    ASSERT_NEAR(d, 100.0, 1.0);
}

TEST(backoff_calculate_stateless)
{
    ShBackoffConfig config = { .base_delay_ms = 100.0, .max_delay_ms = 10000.0, .max_retries = 5, .jitter_factor = 0.0 };

    double d0 = sh_backoff_calculate_seeded(&config, 0, 12345);
    double d1 = sh_backoff_calculate_seeded(&config, 1, 12345);
    double d2 = sh_backoff_calculate_seeded(&config, 2, 12345);

    ASSERT_NEAR(d0, 100.0, 1.0);
    ASSERT_NEAR(d1, 200.0, 1.0);
    ASSERT_NEAR(d2, 400.0, 1.0);
}

TEST(backoff_jitter_range)
{
    ShBackoffConfig config = { .base_delay_ms = 1000.0, .max_delay_ms = 10000.0, .max_retries = 5, .jitter_factor = 0.5 };

    /* With 50% jitter, delay should be in range [500, 1500] for attempt 0 */
    for (int i = 0; i < 20; i++) {
        double d = sh_backoff_calculate_seeded(&config, 0, (uint64_t)(i * 9999 + 1));
        ASSERT(d >= 500.0);
        ASSERT(d <= 1500.0);
    }
}

/* ============================================================================
 * Retry Tests
 * ============================================================================ */

TEST(retry_init)
{
    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, NULL);

    ASSERT_EQ(sh_retry_should_attempt(&ctx), 1);
    ASSERT_EQ(sh_retry_get_last_status(&ctx), 0);
}

TEST(retry_success_no_retry)
{
    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, NULL);

    sh_retry_should_attempt(&ctx);
    double delay = sh_retry_after_response(&ctx, 200, 0);

    ASSERT_NEAR(delay, 0.0, 0.1);
    ASSERT_EQ(sh_retry_should_continue(&ctx), 0);
}

TEST(retry_retryable_status)
{
    ASSERT_EQ(sh_retry_is_retryable_status(429), 1);
    ASSERT_EQ(sh_retry_is_retryable_status(500), 1);
    ASSERT_EQ(sh_retry_is_retryable_status(502), 1);
    ASSERT_EQ(sh_retry_is_retryable_status(503), 1);
    ASSERT_EQ(sh_retry_is_retryable_status(504), 1);
    ASSERT_EQ(sh_retry_is_retryable_status(400), 0);
    ASSERT_EQ(sh_retry_is_retryable_status(401), 0);
    ASSERT_EQ(sh_retry_is_retryable_status(404), 0);
    ASSERT_EQ(sh_retry_is_retryable_status(505), 0);
}

TEST(retry_on_503)
{
    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, NULL);

    sh_retry_should_attempt(&ctx);
    double delay = sh_retry_after_response(&ctx, 503, 0);

    ASSERT(delay > 0);
    ASSERT_EQ(sh_retry_should_continue(&ctx), 1);
}

TEST(retry_honors_retry_after)
{
    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, NULL);

    sh_retry_should_attempt(&ctx);
    double delay = sh_retry_after_response(&ctx, 429, 5.0);  /* 5 seconds */

    ASSERT_NEAR(delay, 5000.0, 100.0);  /* Should use Retry-After */
}

TEST(retry_caps_retry_after)
{
    ShRetryConfig config = SH_RETRY_DEFAULT_CONFIG;
    config.retry_after_max_ms = 2000.0;  /* Cap at 2s */

    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, &config);

    sh_retry_should_attempt(&ctx);
    double delay = sh_retry_after_response(&ctx, 429, 60.0);  /* 60 seconds */

    /* Should use exponential backoff, not the excessive Retry-After */
    ASSERT(delay < 2000.0);
}

TEST(retry_exhausts_retries)
{
    ShRetryConfig config = SH_RETRY_DEFAULT_CONFIG;
    config.backoff.max_retries = 2;

    ShRetryContext ctx;
    sh_retry_init(&ctx, NULL, &config);

    /* First attempt */
    ASSERT_EQ(sh_retry_should_attempt(&ctx), 1);
    sh_retry_after_response(&ctx, 503, 0);
    ASSERT_EQ(sh_retry_should_continue(&ctx), 1);

    /* Second attempt */
    ASSERT_EQ(sh_retry_should_attempt(&ctx), 1);
    sh_retry_after_response(&ctx, 503, 0);
    ASSERT_EQ(sh_retry_should_continue(&ctx), 0);  /* No more retries */

    /* No more attempts */
    ASSERT_EQ(sh_retry_should_attempt(&ctx), 0);
}

TEST(retry_with_circuit)
{
    ShCircuitConfig cc = { .failure_threshold = 2, .success_threshold = 1, .open_duration_ms = 100000.0 };
    ShCircuitBreaker *cb = sh_circuit_create(&cc);

    ShRetryContext ctx;
    sh_retry_init(&ctx, cb, NULL);

    /* Trip the circuit */
    sh_circuit_record(cb, 0);
    sh_circuit_record(cb, 0);
    ASSERT_EQ(sh_circuit_state(cb), SH_CIRCUIT_OPEN);

    /* Retry should not attempt when circuit is open */
    ASSERT_EQ(sh_retry_should_attempt(&ctx), 0);

    sh_circuit_free(cb);
}

/* ============================================================================
 * CORS Tests
 * ============================================================================ */

TEST(cors_init)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    ASSERT_EQ(cors.origin_count, 0);  /* Allow all by default */
    ASSERT_EQ(cors.max_age_seconds, 86400);
}

TEST(cors_add_origin)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    ASSERT_EQ(sh_cors_add_origin(&cors, "https://example.com"), 1);
    ASSERT_EQ(cors.origin_count, 1);
    ASSERT(strcmp(cors.allowed_origins[0], "https://example.com") == 0);
}

TEST(cors_is_allowed_all)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    /* With no origins configured, all are allowed */
    ASSERT_EQ(sh_cors_is_allowed(&cors, "https://any.com"), 1);
    ASSERT_EQ(sh_cors_is_allowed(&cors, "https://other.com"), 1);
}

TEST(cors_is_allowed_whitelist)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);
    sh_cors_add_origin(&cors, "https://allowed.com");

    ASSERT_EQ(sh_cors_is_allowed(&cors, "https://allowed.com"), 1);
    ASSERT_EQ(sh_cors_is_allowed(&cors, "https://other.com"), 0);
}

TEST(cors_headers_wildcard)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    char buf[512];
    int len = sh_cors_headers(&cors, "https://any.com", buf, sizeof(buf));

    ASSERT(len > 0);
    ASSERT(strstr(buf, "Access-Control-Allow-Origin: *") != NULL);
}

TEST(cors_headers_specific)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);
    sh_cors_add_origin(&cors, "https://app.example.com");

    char buf[512];
    int len = sh_cors_headers(&cors, "https://app.example.com", buf, sizeof(buf));

    ASSERT(len > 0);
    ASSERT(strstr(buf, "Access-Control-Allow-Origin: https://app.example.com") != NULL);
}

TEST(cors_preflight_headers)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    char buf[1024];
    int len = sh_cors_preflight_headers(&cors, "https://any.com", buf, sizeof(buf));

    ASSERT(len > 0);
    ASSERT(strstr(buf, "Access-Control-Allow-Methods:") != NULL);
    ASSERT(strstr(buf, "Access-Control-Allow-Headers:") != NULL);
    ASSERT(strstr(buf, "Access-Control-Max-Age:") != NULL);
}

TEST(cors_parse_origins)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);

    int added = sh_cors_parse_origins(&cors, "https://a.com, https://b.com, https://c.com");

    ASSERT_EQ(added, 3);
    ASSERT_EQ(cors.origin_count, 3);
    ASSERT(strcmp(cors.allowed_origins[0], "https://a.com") == 0);
    ASSERT(strcmp(cors.allowed_origins[1], "https://b.com") == 0);
    ASSERT(strcmp(cors.allowed_origins[2], "https://c.com") == 0);
}

TEST(cors_credentials)
{
    ShCorsConfig cors;
    sh_cors_init(&cors);
    sh_cors_add_origin(&cors, "https://app.example.com");
    cors.allow_credentials = 1;

    char buf[512];
    int len = sh_cors_headers(&cors, "https://app.example.com", buf, sizeof(buf));

    ASSERT(len > 0);
    ASSERT(strstr(buf, "Access-Control-Allow-Credentials: true") != NULL);
}

/* ============================================================================
 * Logging Tests
 * ============================================================================ */

TEST(log_init_shutdown)
{
    sh_log_init(NULL);
    ASSERT_EQ(sh_log_get_level(), SH_LOG_LEVEL_INFO);
    sh_log_shutdown();
}

TEST(log_level_from_string)
{
    ASSERT_EQ(sh_log_level_from_string("TRACE"), SH_LOG_LEVEL_TRACE);
    ASSERT_EQ(sh_log_level_from_string("debug"), SH_LOG_LEVEL_DEBUG);
    ASSERT_EQ(sh_log_level_from_string("INFO"), SH_LOG_LEVEL_INFO);
    ASSERT_EQ(sh_log_level_from_string("warn"), SH_LOG_LEVEL_WARN);
    ASSERT_EQ(sh_log_level_from_string("ERROR"), SH_LOG_LEVEL_ERROR);
    ASSERT_EQ(sh_log_level_from_string("fatal"), SH_LOG_LEVEL_FATAL);
    ASSERT_EQ(sh_log_level_from_string("invalid"), SH_LOG_LEVEL_INFO);  /* Default */
}

TEST(log_level_to_string)
{
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_TRACE), "TRACE") == 0);
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_DEBUG), "DEBUG") == 0);
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_INFO), "INFO") == 0);
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_WARN), "WARN") == 0);
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_ERROR), "ERROR") == 0);
    ASSERT(strcmp(sh_log_level_to_string(SH_LOG_LEVEL_FATAL), "FATAL") == 0);
}

TEST(log_set_level)
{
    sh_log_init(NULL);
    sh_log_set_level(SH_LOG_LEVEL_DEBUG);
    ASSERT_EQ(sh_log_get_level(), SH_LOG_LEVEL_DEBUG);
    sh_log_set_level(SH_LOG_LEVEL_ERROR);
    ASSERT_EQ(sh_log_get_level(), SH_LOG_LEVEL_ERROR);
    sh_log_shutdown();
}

TEST(log_enabled)
{
    sh_log_init(NULL);  /* Default: INFO */
    ASSERT_EQ(sh_log_enabled(SH_LOG_LEVEL_TRACE), 0);
    ASSERT_EQ(sh_log_enabled(SH_LOG_LEVEL_DEBUG), 0);
    ASSERT_EQ(sh_log_enabled(SH_LOG_LEVEL_INFO), 1);
    ASSERT_EQ(sh_log_enabled(SH_LOG_LEVEL_WARN), 1);
    ASSERT_EQ(sh_log_enabled(SH_LOG_LEVEL_ERROR), 1);
    sh_log_shutdown();
}

TEST(log_trace_id)
{
    sh_log_init(NULL);
    ASSERT(sh_log_get_trace_id() == NULL);

    sh_log_set_trace_id("test-trace-123");
    ASSERT(sh_log_get_trace_id() != NULL);
    ASSERT(strcmp(sh_log_get_trace_id(), "test-trace-123") == 0);

    sh_log_set_trace_id(NULL);
    ASSERT(sh_log_get_trace_id() == NULL);
    sh_log_shutdown();
}

TEST(log_format_from_string)
{
    ASSERT_EQ(sh_log_format_from_string("json"), SH_LOG_FORMAT_JSON);
    ASSERT_EQ(sh_log_format_from_string("JSON"), SH_LOG_FORMAT_JSON);
    ASSERT_EQ(sh_log_format_from_string("text"), SH_LOG_FORMAT_TEXT);
    ASSERT_EQ(sh_log_format_from_string("invalid"), SH_LOG_FORMAT_TEXT);
}

/* ============================================================================
 * Trace Tests
 * ============================================================================ */

TEST(trace_generate)
{
    char buf[SH_TRACE_ID_LEN];
    char *result = sh_trace_generate(buf);
    ASSERT(result == buf);
    ASSERT(strlen(buf) == 36);
    ASSERT(buf[8] == '-');
    ASSERT(buf[13] == '-');
    ASSERT(buf[14] == '4');  /* UUID v4 */
    ASSERT(buf[18] == '-');
    ASSERT(buf[23] == '-');
}

TEST(trace_validate)
{
    ASSERT_EQ(sh_trace_validate("12345678-1234-4123-8123-123456789abc"), 1);
    ASSERT_EQ(sh_trace_validate("12345678123441238123123456789abc"), 1);  /* Short hex */
    ASSERT_EQ(sh_trace_validate("invalid"), 0);
    ASSERT_EQ(sh_trace_validate(NULL), 0);
    ASSERT_EQ(sh_trace_validate(""), 0);
    ASSERT_EQ(sh_trace_validate("12345"), 0);  /* Too short */
}

TEST(trace_set_get)
{
    sh_trace_clear();
    ASSERT(sh_trace_get() == NULL);

    sh_trace_set("test-trace-456");
    ASSERT(sh_trace_get() != NULL);
    ASSERT(strcmp(sh_trace_get(), "test-trace-456") == 0);

    sh_trace_clear();
    ASSERT(sh_trace_get() == NULL);
}

TEST(trace_new)
{
    sh_trace_clear();
    const char *id1 = sh_trace_new();
    ASSERT(id1 != NULL);
    ASSERT(strlen(id1) == 36);

    /* Copy first ID since sh_trace_new() returns pointer to thread-local storage */
    char id1_copy[SH_TRACE_ID_LEN];
    strncpy(id1_copy, id1, SH_TRACE_ID_LEN);

    const char *id2 = sh_trace_new();
    ASSERT(id2 != NULL);
    ASSERT(strcmp(id1_copy, id2) != 0);  /* Different each time */

    sh_trace_clear();
}

TEST(trace_format_header)
{
    sh_trace_set("abc12345-1234-4123-8123-123456789def");
    char buf[128];
    int len = sh_trace_format_header(buf, sizeof(buf));
    ASSERT(len > 0);
    ASSERT(strstr(buf, "X-Trace-Id:") != NULL);
    ASSERT(strstr(buf, "abc12345") != NULL);
    sh_trace_clear();
}

TEST(trace_span_basic)
{
    sh_log_init(NULL);
    sh_log_set_level(SH_LOG_LEVEL_OFF);  /* Suppress output during test */

    ShTraceSpan span;
    sh_trace_span_start(&span, "test_operation");
    ASSERT(span.span_id[0] != '\0');
    ASSERT(span.operation != NULL);
    ASSERT(strcmp(span.operation, "test_operation") == 0);

    sh_trace_span_end(&span);
    sh_trace_clear();
    sh_log_shutdown();
}

/* ============================================================================
 * Metrics Tests
 * ============================================================================ */

TEST(metrics_init_shutdown)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    ASSERT_EQ(sh_metrics_init(&cfg), 0);
    ASSERT_EQ(sh_metrics_count(), 0);
    sh_metrics_shutdown();
}

TEST(metrics_counter)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    sh_metrics_counter_inc("requests_total", 1, NULL);
    sh_metrics_counter_inc("requests_total", 5, NULL);

    const ShMetric *m = sh_metrics_get("requests_total", "");
    ASSERT(m != NULL);
    ASSERT_EQ(m->type, SH_METRIC_COUNTER);
    ASSERT_EQ(m->value.counter, 6);

    sh_metrics_shutdown();
}

TEST(metrics_gauge)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    sh_metrics_gauge_set("connections", 42.0, NULL);

    const ShMetric *m = sh_metrics_get("connections", "");
    ASSERT(m != NULL);
    ASSERT_EQ(m->type, SH_METRIC_GAUGE);
    ASSERT_NEAR(m->value.gauge, 42.0, 0.1);

    sh_metrics_gauge_set("connections", 100.0, NULL);
    ASSERT_NEAR(m->value.gauge, 100.0, 0.1);

    sh_metrics_shutdown();
}

TEST(metrics_histogram)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    sh_metrics_histogram_observe("latency_ms", 10.0, NULL);
    sh_metrics_histogram_observe("latency_ms", 20.0, NULL);
    sh_metrics_histogram_observe("latency_ms", 30.0, NULL);

    const ShMetric *m = sh_metrics_get("latency_ms", "");
    ASSERT(m != NULL);
    ASSERT_EQ(m->type, SH_METRIC_HISTOGRAM);
    ASSERT_EQ(m->value.histogram.count, 3);
    ASSERT_NEAR(m->value.histogram.sum, 60.0, 0.1);

    sh_metrics_shutdown();
}

TEST(metrics_timer)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    ShMetricsTimer timer = sh_metrics_timer_start();
    /* Small delay to ensure measurable time */
    for (volatile int i = 0; i < 100000; i++);
    sh_metrics_timer_observe(timer, "operation_time_ms", NULL);

    const ShMetric *m = sh_metrics_get("operation_time_ms", "");
    ASSERT(m != NULL);
    ASSERT_EQ(m->value.histogram.count, 1);
    ASSERT(m->value.histogram.sum >= 0);  /* Some time elapsed */

    sh_metrics_shutdown();
}

TEST(metrics_prometheus_output)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "myapp";
    sh_metrics_init(&cfg);

    sh_metrics_counter_inc("requests", 10, NULL);
    sh_metrics_gauge_set("temp", 25.5, NULL);

    char *output = sh_metrics_prometheus_output();
    ASSERT(output != NULL);
    ASSERT(strstr(output, "myapp_requests") != NULL);
    ASSERT(strstr(output, "counter") != NULL);
    ASSERT(strstr(output, "10") != NULL);
    free(output);

    sh_metrics_shutdown();
}

TEST(metrics_with_tags)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    sh_metrics_counter_inc("http_requests", 1, "method:GET", "status:200", NULL);
    sh_metrics_counter_inc("http_requests", 1, "method:POST", "status:201", NULL);

    ASSERT_EQ(sh_metrics_count(), 2);  /* Two different tag combinations */

    sh_metrics_shutdown();
}

TEST(metrics_http_request)
{
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";
    sh_metrics_init(&cfg);

    sh_metrics_http_request("GET", "/api/health", 200, 15.5, 0, 256);
    sh_metrics_http_request("POST", "/api/data", 201, 45.0, 1024, 512);

    /* Should have created multiple metrics */
    ASSERT(sh_metrics_count() > 0);

    sh_metrics_shutdown();
}

TEST(metrics_statsd_hostname_resolution)
{
    /*
     * Test that statsd connection works with different hostname formats.
     * Uses getaddrinfo internally (thread-safe replacement for gethostbyname).
     */
    ShMetricsConfig cfg = SH_METRICS_CONFIG_DEFAULT;
    cfg.service = "test";

    /* Test 1: IP address should work */
    cfg.statsd_host = "127.0.0.1";
    cfg.statsd_port = 8125;
    ASSERT_EQ(sh_metrics_init(&cfg), 0);
    /* Send a metric - won't fail even if no server is listening (UDP) */
    sh_metrics_counter_inc("test_counter", 1, NULL);
    sh_metrics_shutdown();

    /* Test 2: "localhost" hostname should resolve */
    cfg.statsd_host = "localhost";
    ASSERT_EQ(sh_metrics_init(&cfg), 0);
    sh_metrics_counter_inc("test_counter", 1, NULL);
    sh_metrics_shutdown();

    /* Test 3: NULL host should work (no statsd connection) */
    cfg.statsd_host = NULL;
    ASSERT_EQ(sh_metrics_init(&cfg), 0);
    sh_metrics_counter_inc("test_counter", 1, NULL);
    sh_metrics_shutdown();
}

/* ============================================================================
 * Hashmap Tests
 * ============================================================================ */

TEST(hashmap_i64_create_free)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);
    ASSERT(map != NULL);
    ASSERT_EQ(sh_hashmap_i64_count(map), 0);
    ASSERT(sh_hashmap_i64_capacity(map) >= 100);
    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_insert_lookup)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);
    ASSERT(map != NULL);

    ASSERT_EQ(sh_hashmap_i64_insert(map, 12345, 100), SH_HASHMAP_OK);
    ASSERT_EQ(sh_hashmap_i64_insert(map, 67890, 200), SH_HASHMAP_OK);
    ASSERT_EQ(sh_hashmap_i64_insert(map, -99999, 300), SH_HASHMAP_OK);

    ASSERT_EQ(sh_hashmap_i64_count(map), 3);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 12345), 100);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 67890), 200);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, -99999), 300);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 11111), SIZE_MAX);  /* Not found */

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_update)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);

    sh_hashmap_i64_insert(map, 123, 10);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 123), 10);

    sh_hashmap_i64_insert(map, 123, 20);  /* Update */
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 123), 20);
    ASSERT_EQ(sh_hashmap_i64_count(map), 1);  /* Still 1 entry */

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_contains)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);

    ASSERT(!sh_hashmap_i64_contains(map, 123));
    sh_hashmap_i64_insert(map, 123, 456);
    ASSERT(sh_hashmap_i64_contains(map, 123));
    ASSERT(!sh_hashmap_i64_contains(map, 789));

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_zero_key_rejected)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);

    ASSERT_EQ(sh_hashmap_i64_insert(map, 0, 123), SH_HASHMAP_ERROR_INVALID_KEY);
    ASSERT_EQ(sh_hashmap_i64_count(map), 0);

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_clear)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);

    sh_hashmap_i64_insert(map, 1, 10);
    sh_hashmap_i64_insert(map, 2, 20);
    sh_hashmap_i64_insert(map, 3, 30);
    ASSERT_EQ(sh_hashmap_i64_count(map), 3);

    sh_hashmap_i64_clear(map);
    ASSERT_EQ(sh_hashmap_i64_count(map), 0);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 1), SIZE_MAX);

    /* Can reuse after clear */
    sh_hashmap_i64_insert(map, 100, 1000);
    ASSERT_EQ(sh_hashmap_i64_lookup(map, 100), 1000);

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_resize)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(10);
    size_t initial_cap = sh_hashmap_i64_capacity(map);

    /* Insert enough entries to trigger resize */
    for (int64_t i = 1; i <= 100; i++) {
        ASSERT_EQ(sh_hashmap_i64_insert(map, i, (size_t)i * 10), SH_HASHMAP_OK);
    }

    ASSERT(sh_hashmap_i64_capacity(map) > initial_cap);
    ASSERT_EQ(sh_hashmap_i64_count(map), 100);

    /* Verify all entries still accessible */
    for (int64_t i = 1; i <= 100; i++) {
        ASSERT_EQ(sh_hashmap_i64_lookup(map, i), (size_t)i * 10);
    }

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64_iterator)
{
    SHHashmapI64 *map = sh_hashmap_i64_create(100);

    sh_hashmap_i64_insert(map, 10, 100);
    sh_hashmap_i64_insert(map, 20, 200);
    sh_hashmap_i64_insert(map, 30, 300);

    SHHashmapI64Iter iter;
    sh_hashmap_i64_iter_init(&iter, map);

    int count = 0;
    int64_t key;
    size_t value;
    int64_t sum_keys = 0;
    size_t sum_values = 0;

    while (sh_hashmap_i64_iter_next(&iter, &key, &value)) {
        count++;
        sum_keys += key;
        sum_values += value;
    }

    ASSERT_EQ(count, 3);
    ASSERT_EQ(sum_keys, 60);  /* 10 + 20 + 30 */
    ASSERT_EQ(sum_values, 600);  /* 100 + 200 + 300 */

    sh_hashmap_i64_free(map);
}

TEST(hashmap_i64u32_basic)
{
    SHHashmapI64U32 *map = sh_hashmap_i64u32_create(100);
    ASSERT(map != NULL);

    ASSERT_EQ(sh_hashmap_i64u32_insert(map, 12345, 100), SH_HASHMAP_OK);
    ASSERT_EQ(sh_hashmap_i64u32_insert(map, 67890, 200), SH_HASHMAP_OK);

    ASSERT_EQ(sh_hashmap_i64u32_lookup(map, 12345), 100);
    ASSERT_EQ(sh_hashmap_i64u32_lookup(map, 67890), 200);
    ASSERT_EQ(sh_hashmap_i64u32_lookup(map, 11111), UINT32_MAX);

    sh_hashmap_i64u32_free(map);
}

TEST(hashmap_null_safety)
{
    ASSERT(sh_hashmap_i64_create(0) != NULL);  /* Creates with default capacity */

    sh_hashmap_i64_free(NULL);  /* Should not crash */

    ASSERT_EQ(sh_hashmap_i64_insert(NULL, 1, 1), SH_HASHMAP_ERROR_NULL_PARAM);
    ASSERT_EQ(sh_hashmap_i64_lookup(NULL, 1), SIZE_MAX);
    ASSERT_EQ(sh_hashmap_i64_count(NULL), 0);

    SHHashmapI64 *map = sh_hashmap_i64_create(10);
    sh_hashmap_i64_free(map);
}

/* ============================================================================
 * Heap Tests
 * ============================================================================ */

TEST(heap_create_free)
{
    SHHeap *heap = sh_heap_create(1000);
    ASSERT(heap != NULL);
    ASSERT(sh_heap_empty(heap));
    ASSERT_EQ(sh_heap_size(heap), 0);
    sh_heap_free(heap);
}

TEST(heap_push_pop)
{
    SHHeap *heap = sh_heap_create(100);

    ASSERT_EQ(sh_heap_push(heap, 5, 50.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_push(heap, 3, 30.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_push(heap, 7, 70.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_push(heap, 1, 10.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_push(heap, 9, 90.0), SH_HEAP_OK);

    ASSERT_EQ(sh_heap_size(heap), 5);

    /* Pop should return in priority order (min first) */
    SHHeapEntry entry;
    ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 1);
    ASSERT_NEAR(entry.priority, 10.0, 0.001);

    ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 3);

    ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 5);

    ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 7);

    ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 9);

    ASSERT(sh_heap_empty(heap));

    sh_heap_free(heap);
}

TEST(heap_peek)
{
    SHHeap *heap = sh_heap_create(100);

    sh_heap_push(heap, 5, 50.0);
    sh_heap_push(heap, 3, 30.0);
    sh_heap_push(heap, 7, 70.0);

    SHHeapEntry entry;
    ASSERT_EQ(sh_heap_peek(heap, &entry), SH_HEAP_OK);
    ASSERT_EQ(entry.node, 3);  /* Minimum */
    ASSERT_EQ(sh_heap_size(heap), 3);  /* Not removed */

    sh_heap_free(heap);
}

TEST(heap_contains_priority)
{
    SHHeap *heap = sh_heap_create(100);

    ASSERT(!sh_heap_contains(heap, 5));
    sh_heap_push(heap, 5, 50.0);
    ASSERT(sh_heap_contains(heap, 5));
    ASSERT_NEAR(sh_heap_priority(heap, 5), 50.0, 0.001);

    /* Node not in heap returns infinity */
    ASSERT(!sh_heap_contains(heap, 99));
    ASSERT(sh_heap_priority(heap, 99) > 1e300);

    sh_heap_free(heap);
}

TEST(heap_decrease_key)
{
    SHHeap *heap = sh_heap_create(100);

    sh_heap_push(heap, 5, 50.0);
    sh_heap_push(heap, 3, 30.0);
    sh_heap_push(heap, 7, 70.0);

    /* Decrease key of node 7 to make it minimum */
    ASSERT_EQ(sh_heap_decrease_key(heap, 7, 10.0), SH_HEAP_OK);
    ASSERT_NEAR(sh_heap_priority(heap, 7), 10.0, 0.001);

    SHHeapEntry entry;
    sh_heap_pop(heap, &entry);
    ASSERT_EQ(entry.node, 7);  /* Now minimum */

    sh_heap_free(heap);
}

TEST(heap_decrease_key_noop)
{
    SHHeap *heap = sh_heap_create(100);

    sh_heap_push(heap, 5, 50.0);

    /* Try to increase priority (should be ignored) */
    sh_heap_decrease_key(heap, 5, 100.0);
    ASSERT_NEAR(sh_heap_priority(heap, 5), 50.0, 0.001);

    sh_heap_free(heap);
}

TEST(heap_push_or_decrease)
{
    SHHeap *heap = sh_heap_create(100);

    /* Push new */
    ASSERT_EQ(sh_heap_push_or_decrease(heap, 5, 50.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_size(heap), 1);

    /* Update existing */
    ASSERT_EQ(sh_heap_push_or_decrease(heap, 5, 25.0), SH_HEAP_OK);
    ASSERT_EQ(sh_heap_size(heap), 1);
    ASSERT_NEAR(sh_heap_priority(heap, 5), 25.0, 0.001);

    sh_heap_free(heap);
}

TEST(heap_clear)
{
    SHHeap *heap = sh_heap_create(100);

    sh_heap_push(heap, 1, 10.0);
    sh_heap_push(heap, 2, 20.0);
    sh_heap_push(heap, 3, 30.0);
    ASSERT_EQ(sh_heap_size(heap), 3);

    sh_heap_clear(heap);
    ASSERT(sh_heap_empty(heap));
    ASSERT(!sh_heap_contains(heap, 1));

    /* Can reuse after clear */
    sh_heap_push(heap, 10, 100.0);
    ASSERT_EQ(sh_heap_size(heap), 1);

    sh_heap_free(heap);
}

TEST(heap_many_entries)
{
    SHHeap *heap = sh_heap_create(10000);

    /* Insert in reverse order */
    for (uint32_t i = 1000; i > 0; i--) {
        sh_heap_push(heap, i - 1, (double)(i - 1));
    }

    ASSERT_EQ(sh_heap_size(heap), 1000);

    /* Pop should return in order */
    SHHeapEntry entry;
    for (uint32_t i = 0; i < 1000; i++) {
        ASSERT_EQ(sh_heap_pop(heap, &entry), SH_HEAP_OK);
        ASSERT_EQ(entry.node, i);
    }

    sh_heap_free(heap);
}

TEST(heap_null_safety)
{
    sh_heap_free(NULL);  /* Should not crash */

    ASSERT(sh_heap_empty(NULL));
    ASSERT_EQ(sh_heap_size(NULL), 0);
    ASSERT(!sh_heap_contains(NULL, 0));

    ASSERT_EQ(sh_heap_push(NULL, 1, 1.0), SH_HEAP_ERROR_NULL_PARAM);
    ASSERT_EQ(sh_heap_pop(NULL, NULL), SH_HEAP_ERROR_NULL_PARAM);

    SHHeap *heap = sh_heap_create(10);
    ASSERT_EQ(sh_heap_pop(heap, NULL), SH_HEAP_ERROR_EMPTY);
    sh_heap_free(heap);
}

/* ============================================================================
 * Spatial Grid Tests
 * ============================================================================ */

TEST(dynamic_grid_create_free)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHDynamicGrid *grid = sh_dynamic_grid_create(bounds, 0.1);
    ASSERT(grid != NULL);
    ASSERT_EQ(sh_dynamic_grid_num_entries(grid), 0);
    ASSERT(sh_dynamic_grid_num_cells(grid) > 0);
    sh_dynamic_grid_free(grid);
}

TEST(dynamic_grid_insert_query)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHDynamicGrid *grid = sh_dynamic_grid_create(bounds, 0.5);

    /* Insert some points */
    SHCoord p1 = {47.0, 19.0};
    SHCoord p2 = {47.1, 19.1};
    SHCoord p3 = {48.0, 20.0};

    ASSERT_EQ(sh_dynamic_grid_insert(grid, p1, 100), SH_GRID_OK);
    ASSERT_EQ(sh_dynamic_grid_insert(grid, p2, 101), SH_GRID_OK);
    ASSERT_EQ(sh_dynamic_grid_insert(grid, p3, 200), SH_GRID_OK);

    ASSERT_EQ(sh_dynamic_grid_num_entries(grid), 3);

    /* Query same cell as p1 */
    uint32_t results[10];
    size_t count = sh_dynamic_grid_query_point(grid, p1, 10, results);
    ASSERT(count >= 1);  /* At least p1 should be there */

    sh_dynamic_grid_free(grid);
}

TEST(dynamic_grid_radius_query)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHDynamicGrid *grid = sh_dynamic_grid_create(bounds, 0.1);

    /* Insert cluster of points */
    SHCoord center = {47.0, 19.0};
    sh_dynamic_grid_insert(grid, center, 0);
    sh_dynamic_grid_insert(grid, (SHCoord){47.01, 19.0}, 1);
    sh_dynamic_grid_insert(grid, (SHCoord){47.0, 19.01}, 2);
    sh_dynamic_grid_insert(grid, (SHCoord){47.02, 19.02}, 3);

    /* Far away point */
    sh_dynamic_grid_insert(grid, (SHCoord){48.0, 20.0}, 99);

    uint32_t results[10];
    size_t count = sh_dynamic_grid_query_radius(grid, center, 5000, 10, results);  /* 5km radius */
    ASSERT(count >= 4);  /* Should find the cluster */

    sh_dynamic_grid_free(grid);
}

TEST(dynamic_grid_out_of_bounds)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHDynamicGrid *grid = sh_dynamic_grid_create(bounds, 0.1);

    /* Insert out-of-bounds point (should be silently ignored) */
    SHCoord outside = {60.0, 30.0};
    ASSERT_EQ(sh_dynamic_grid_insert(grid, outside, 999), SH_GRID_OK);
    ASSERT_EQ(sh_dynamic_grid_num_entries(grid), 0);

    sh_dynamic_grid_free(grid);
}

TEST(dynamic_grid_duplicate_detection)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHDynamicGrid *grid = sh_dynamic_grid_create(bounds, 0.1);

    SHCoord p = {47.0, 19.0};
    sh_dynamic_grid_insert(grid, p, 100);
    sh_dynamic_grid_insert(grid, p, 100);  /* Same entity */
    sh_dynamic_grid_insert(grid, p, 101);  /* Different entity */

    ASSERT_EQ(sh_dynamic_grid_num_entries(grid), 2);  /* No duplicate */

    sh_dynamic_grid_free(grid);
}

TEST(csr_grid_create_free)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};

    SHCoord coords[] = {
        {47.0, 19.0},
        {47.5, 19.5},
        {48.0, 20.0}
    };

    SHCSRGrid *grid = sh_csr_grid_create_f(bounds, 0.5, coords, 3);
    ASSERT(grid != NULL);
    sh_csr_grid_free(grid);
}

TEST(csr_grid_query_cell)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};

    SHCoord coords[] = {
        {47.0, 19.0},   /* 0 */
        {47.1, 19.1},   /* 1 - same cell as 0 */
        {48.0, 20.0}    /* 2 - different cell */
    };

    SHCSRGrid *grid = sh_csr_grid_create_f(bounds, 0.5, coords, 3);

    const uint32_t *start, *end;
    SHCoord query = {47.05, 19.05};  /* In same cell as points 0 and 1 */

    ASSERT_EQ(sh_csr_grid_query_cell(grid, query, &start, &end), 0);
    ASSERT(end - start >= 2);  /* At least 2 points in this cell */

    sh_csr_grid_free(grid);
}

/* Distance callback for nearest neighbor tests */
static double test_distance_cb(SHCoord query, uint32_t node_id, void *user_data)
{
    SHCoord *coords = (SHCoord *)user_data;
    return sh_haversine(query, coords[node_id]);
}

TEST(csr_grid_find_nearest)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};

    SHCoord coords[] = {
        {47.0, 19.0},   /* 0 - ~11km from query */
        {47.5, 19.5},   /* 1 - closest to query */
        {48.0, 20.0}    /* 2 - far */
    };

    SHCSRGrid *grid = sh_csr_grid_create_f(bounds, 0.1, coords, 3);

    SHCoord query = {47.51, 19.51};  /* Near point 1 */
    double dist;
    uint32_t nearest = sh_csr_grid_find_nearest(grid, query, test_distance_cb, coords, &dist);

    ASSERT_EQ(nearest, 1);
    ASSERT(dist < 5000);  /* Should be within 5km */

    sh_csr_grid_free(grid);
}

TEST(csr_grid_cell_index)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHCSRGrid *grid = sh_csr_grid_create_f(bounds, 1.0, NULL, 0);

    SHCoord inside = {47.0, 19.0};
    SHCoord outside = {60.0, 30.0};

    ASSERT(sh_csr_grid_cell_index(grid, inside) >= 0);
    ASSERT(sh_csr_grid_cell_index(grid, outside) < 0);

    sh_csr_grid_free(grid);
}

TEST(grid_config_create)
{
    SHBBox bounds = {.min_lat = 45.0, .max_lat = 50.0, .min_lon = 15.0, .max_lon = 25.0};
    SHGridConfig config = sh_grid_config_create(bounds, 0.5);

    ASSERT_EQ(config.grid_height, 10);  /* 5 degrees / 0.5 = 10 */
    ASSERT_EQ(config.grid_width, 20);   /* 10 degrees / 0.5 = 20 */
    ASSERT_NEAR(config.cell_size_lat, 0.5, 0.001);
}

TEST(spatial_grid_null_safety)
{
    sh_dynamic_grid_free(NULL);  /* Should not crash */
    sh_csr_grid_free(NULL);

    ASSERT_EQ(sh_dynamic_grid_num_entries(NULL), 0);
    ASSERT_EQ(sh_dynamic_grid_num_cells(NULL), 0);

    ASSERT_EQ(sh_dynamic_grid_insert(NULL, (SHCoord){0, 0}, 0), SH_GRID_ERROR_NULL_PARAM);
}

/* ============================================================================
 * Query String Parsing Tests
 * ============================================================================ */

TEST(query_get_int_basic)
{
    ASSERT_EQ(sh_query_get_int("width=80&height=40", "width", 0), 80);
    ASSERT_EQ(sh_query_get_int("width=80&height=40", "height", 0), 40);
}

TEST(query_get_int_default)
{
    ASSERT_EQ(sh_query_get_int("width=80", "height", 99), 99);
    ASSERT_EQ(sh_query_get_int(NULL, "width", 42), 42);
    ASSERT_EQ(sh_query_get_int("", "width", 42), 42);
}

TEST(query_get_int_edge_cases)
{
    /* First parameter */
    ASSERT_EQ(sh_query_get_int("x=5", "x", 0), 5);
    /* Last parameter without & */
    ASSERT_EQ(sh_query_get_int("a=1&b=2&c=3", "c", 0), 3);
    /* Negative numbers */
    ASSERT_EQ(sh_query_get_int("x=-42", "x", 0), -42);
}

TEST(query_get_str_basic)
{
    char buf[64];
    ASSERT_EQ(sh_query_get_str("name=hello&type=test", "name", buf, sizeof(buf)), 5);
    ASSERT(strcmp(buf, "hello") == 0);
    ASSERT_EQ(sh_query_get_str("name=hello&type=test", "type", buf, sizeof(buf)), 4);
    ASSERT(strcmp(buf, "test") == 0);
}

TEST(query_get_str_not_found)
{
    char buf[64];
    buf[0] = 'X';
    ASSERT_EQ(sh_query_get_str("name=hello", "missing", buf, sizeof(buf)), 0);
    ASSERT(buf[0] == '\0');  /* Buffer cleared even on not found */
}

TEST(query_get_str_null_safety)
{
    char buf[64];
    ASSERT_EQ(sh_query_get_str(NULL, "key", buf, sizeof(buf)), 0);
    ASSERT_EQ(sh_query_get_str("key=val", NULL, buf, sizeof(buf)), 0);
    ASSERT_EQ(sh_query_get_str("key=val", "key", NULL, sizeof(buf)), 0);
    ASSERT_EQ(sh_query_get_str("key=val", "key", buf, 0), 0);
}

TEST(query_has_basic)
{
    ASSERT_EQ(sh_query_has("debug=1&verbose", "debug"), 1);
    ASSERT_EQ(sh_query_has("debug=1&verbose", "verbose"), 1);
    ASSERT_EQ(sh_query_has("debug=1&verbose", "missing"), 0);
}

TEST(query_has_null_safety)
{
    ASSERT_EQ(sh_query_has(NULL, "key"), 0);
    ASSERT_EQ(sh_query_has("key=val", NULL), 0);
}

TEST(query_get_double_basic)
{
    ASSERT_NEAR(sh_query_get_double("lat=47.5&lon=19.1", "lat", 0.0), 47.5, 0.0001);
    ASSERT_NEAR(sh_query_get_double("lat=47.5&lon=19.1", "lon", 0.0), 19.1, 0.0001);
}

TEST(query_get_double_default)
{
    ASSERT_NEAR(sh_query_get_double("x=1.5", "y", 99.9), 99.9, 0.0001);
    ASSERT_NEAR(sh_query_get_double(NULL, "x", 42.0), 42.0, 0.0001);
}

/* ============================================================================
 * Render Tests
 * ============================================================================ */

TEST(render_set_get_pixel)
{
    /* Create a 4x4 RGBA buffer */
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Set a pixel */
    sh_set_pixel(pixels, 4, 4, 1, 2, SH_RGBA(255, 128, 64, 255));

    /* Get it back */
    uint32_t c = sh_get_pixel(pixels, 4, 4, 1, 2);
    ASSERT_EQ(SH_COLOR_R(c), 255);
    ASSERT_EQ(SH_COLOR_G(c), 128);
    ASSERT_EQ(SH_COLOR_B(c), 64);
    ASSERT_EQ(SH_COLOR_A(c), 255);
}

TEST(render_get_pixel_out_of_bounds)
{
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0xFF, sizeof(pixels));

    /* Out of bounds should return 0 */
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, -1, 0), 0);
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, 0, -1), 0);
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, 4, 0), 0);
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, 0, 4), 0);
}

TEST(render_set_pixel_out_of_bounds)
{
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Out of bounds set should be no-op */
    sh_set_pixel(pixels, 4, 4, -1, 0, SH_RGBA(255, 255, 255, 255));
    sh_set_pixel(pixels, 4, 4, 4, 0, SH_RGBA(255, 255, 255, 255));

    /* Verify no pixels were modified */
    for (int i = 0; i < 4 * 4 * 4; i++) {
        ASSERT_EQ(pixels[i], 0);
    }
}

TEST(render_blend_pixel_opaque)
{
    uint8_t pixels[4] = {100, 100, 100, 255};  /* Gray background */

    /* Blend fully opaque red */
    sh_blend_pixel_unchecked(pixels, SH_RGBA(255, 0, 0, 255));

    ASSERT_EQ(pixels[0], 255);  /* R */
    ASSERT_EQ(pixels[1], 0);    /* G */
    ASSERT_EQ(pixels[2], 0);    /* B */
    ASSERT_EQ(pixels[3], 255);  /* A */
}

TEST(render_blend_pixel_transparent)
{
    uint8_t pixels[4] = {100, 100, 100, 255};

    /* Blend fully transparent - should not change */
    sh_blend_pixel_unchecked(pixels, SH_RGBA(255, 0, 0, 0));

    ASSERT_EQ(pixels[0], 100);
    ASSERT_EQ(pixels[1], 100);
    ASSERT_EQ(pixels[2], 100);
    ASSERT_EQ(pixels[3], 255);
}

TEST(render_blend_pixel_50_percent)
{
    uint8_t pixels[4] = {0, 0, 0, 255};  /* Black background */

    /* Blend 50% white */
    sh_blend_pixel_unchecked(pixels, SH_RGBA(255, 255, 255, 128));

    /* Should be close to gray */
    ASSERT(pixels[0] > 100 && pixels[0] < 156);  /* R ~128 */
    ASSERT(pixels[1] > 100 && pixels[1] < 156);  /* G ~128 */
    ASSERT(pixels[2] > 100 && pixels[2] < 156);  /* B ~128 */
}

TEST(render_fill_span_opaque)
{
    uint8_t pixels[10 * 4 * 4];  /* 10x4 buffer */
    memset(pixels, 0, sizeof(pixels));

    /* Fill span on row 1 from x=2 to x=5 */
    sh_fill_span(pixels, 10, 4, 1, 2, 5, SH_RGBA(200, 100, 50, 255));

    /* Check pixels on row 1 */
    for (int x = 0; x < 10; x++) {
        uint32_t c = sh_get_pixel(pixels, 10, 4, x, 1);
        if (x >= 2 && x <= 5) {
            ASSERT_EQ(SH_COLOR_R(c), 200);
            ASSERT_EQ(SH_COLOR_G(c), 100);
            ASSERT_EQ(SH_COLOR_B(c), 50);
        } else {
            ASSERT_EQ(c, 0);  /* Not touched */
        }
    }
}

TEST(render_fill_span_clipping)
{
    uint8_t pixels[4 * 2 * 4];  /* 4x2 buffer */
    memset(pixels, 0, sizeof(pixels));

    /* Span extends beyond buffer - should clip */
    sh_fill_span(pixels, 4, 2, 0, -2, 6, SH_RGBA(255, 0, 0, 255));

    /* Only x=0 to x=3 should be filled */
    for (int x = 0; x < 4; x++) {
        uint32_t c = sh_get_pixel(pixels, 4, 2, x, 0);
        ASSERT_EQ(SH_COLOR_R(c), 255);
    }

    /* Row 1 should be untouched */
    for (int x = 0; x < 4; x++) {
        ASSERT_EQ(sh_get_pixel(pixels, 4, 2, x, 1), 0);
    }
}

TEST(render_fill_span_out_of_bounds_y)
{
    uint8_t pixels[4 * 2 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Y out of bounds - should be no-op */
    sh_fill_span(pixels, 4, 2, -1, 0, 3, SH_RGBA(255, 0, 0, 255));
    sh_fill_span(pixels, 4, 2, 2, 0, 3, SH_RGBA(255, 0, 0, 255));

    /* Verify no pixels were modified */
    for (int i = 0; i < 4 * 2 * 4; i++) {
        ASSERT_EQ(pixels[i], 0);
    }
}

TEST(render_fill_span_alpha_blend)
{
    uint8_t pixels[4 * 4];  /* 4x1 buffer */
    memset(pixels, 0, sizeof(pixels));  /* Black background */

    /* Fill with 50% white */
    sh_fill_span(pixels, 4, 1, 0, 0, 3, SH_RGBA(255, 255, 255, 128));

    /* All pixels should be ~gray */
    for (int x = 0; x < 4; x++) {
        uint32_t c = sh_get_pixel(pixels, 4, 1, x, 0);
        ASSERT(SH_COLOR_R(c) > 100 && SH_COLOR_R(c) < 160);
    }
}

TEST(render_clear_buffer)
{
    uint8_t pixels[8 * 8 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Clear to a color */
    sh_clear_buffer(pixels, 8, 8, SH_RGBA(50, 100, 150, 255));

    /* Check all pixels */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint32_t c = sh_get_pixel(pixels, 8, 8, x, y);
            ASSERT_EQ(SH_COLOR_R(c), 50);
            ASSERT_EQ(SH_COLOR_G(c), 100);
            ASSERT_EQ(SH_COLOR_B(c), 150);
            ASSERT_EQ(SH_COLOR_A(c), 255);
        }
    }
}

TEST(render_clear_buffer_uniform)
{
    uint8_t pixels[8 * 8 * 4];

    /* Clear to uniform color (memset fast path) */
    sh_clear_buffer(pixels, 8, 8, SH_RGBA(128, 128, 128, 128));

    for (int i = 0; i < 8 * 8 * 4; i++) {
        ASSERT_EQ(pixels[i], 128);
    }
}

TEST(render_clear_rect)
{
    uint8_t pixels[10 * 10 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Clear a 4x3 rectangle at (2, 3) */
    sh_clear_rect(pixels, 10, 10, 2, 3, 4, 3, SH_RGBA(255, 128, 64, 255));

    /* Check that only the rectangle is filled */
    for (int y = 0; y < 10; y++) {
        for (int x = 0; x < 10; x++) {
            uint32_t c = sh_get_pixel(pixels, 10, 10, x, y);
            if (x >= 2 && x < 6 && y >= 3 && y < 6) {
                ASSERT_EQ(SH_COLOR_R(c), 255);
                ASSERT_EQ(SH_COLOR_G(c), 128);
            } else {
                ASSERT_EQ(c, 0);
            }
        }
    }
}

TEST(render_clear_rect_clipping)
{
    uint8_t pixels[4 * 4 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* Rectangle partially outside buffer */
    sh_clear_rect(pixels, 4, 4, -1, -1, 3, 3, SH_RGBA(255, 0, 0, 255));

    /* Only (0,0), (1,0), (0,1), (1,1) should be filled */
    ASSERT(SH_COLOR_R(sh_get_pixel(pixels, 4, 4, 0, 0)) == 255);
    ASSERT(SH_COLOR_R(sh_get_pixel(pixels, 4, 4, 1, 0)) == 255);
    ASSERT(SH_COLOR_R(sh_get_pixel(pixels, 4, 4, 0, 1)) == 255);
    ASSERT(SH_COLOR_R(sh_get_pixel(pixels, 4, 4, 1, 1)) == 255);
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, 2, 0), 0);
    ASSERT_EQ(sh_get_pixel(pixels, 4, 4, 0, 2), 0);
}

TEST(render_color_macros)
{
    uint32_t c = SH_RGBA(10, 20, 30, 40);
    ASSERT_EQ(SH_COLOR_R(c), 10);
    ASSERT_EQ(SH_COLOR_G(c), 20);
    ASSERT_EQ(SH_COLOR_B(c), 30);
    ASSERT_EQ(SH_COLOR_A(c), 40);

    uint32_t rgb = SH_RGB(100, 150, 200);
    ASSERT_EQ(SH_COLOR_R(rgb), 100);
    ASSERT_EQ(SH_COLOR_G(rgb), 150);
    ASSERT_EQ(SH_COLOR_B(rgb), 200);
    ASSERT_EQ(SH_COLOR_A(rgb), 255);
}

/* ============================================================================
 * Unit Conversion Tests
 * ============================================================================ */

TEST(unit_km_miles_roundtrip)
{
    double km = 100.0;
    double miles = sh_km_to_miles(km);
    double km_back = sh_miles_to_km(miles);
    ASSERT_NEAR(km, km_back, 0.0001);
}

TEST(unit_miles_km_roundtrip)
{
    double miles = 100.0;
    double km = sh_miles_to_km(miles);
    double miles_back = sh_km_to_miles(km);
    ASSERT_NEAR(miles, miles_back, 0.0001);
}

TEST(unit_meters_km)
{
    ASSERT_NEAR(sh_m_to_km(1000.0), 1.0, 0.0001);
    ASSERT_NEAR(sh_km_to_m(1.0), 1000.0, 0.0001);
}

TEST(unit_meters_miles)
{
    /* 1609.344 meters = 1 mile */
    ASSERT_NEAR(sh_m_to_miles(1609.344), 1.0, 0.0001);
    ASSERT_NEAR(sh_miles_to_m(1.0), 1609.344, 0.001);
}

TEST(unit_efficiency_conversion)
{
    /* 30 MPG is approximately 7.84 L/100km */
    double mpg = 30.0;
    double l100km = sh_mpg_to_l100km(mpg);
    ASSERT_NEAR(l100km, 7.84, 0.01);

    /* Round-trip */
    double mpg_back = sh_l100km_to_mpg(l100km);
    ASSERT_NEAR(mpg, mpg_back, 0.001);
}

TEST(unit_efficiency_zero_handling)
{
    /* Zero efficiency should not crash */
    ASSERT_NEAR(sh_mpg_to_l100km(0), 0.0, 0.0001);
    ASSERT_NEAR(sh_l100km_to_mpg(0), 0.0, 0.0001);
}

TEST(unit_volume_conversion)
{
    double gallons = 10.0;
    double liters = sh_gallons_to_liters(gallons);
    ASSERT_NEAR(liters, 37.8541, 0.001);
    ASSERT_NEAR(sh_liters_to_gallons(liters), gallons, 0.0001);
}

TEST(unit_weight_conversion)
{
    double kg = 100.0;
    double lbs = sh_kg_to_lbs(kg);
    ASSERT_NEAR(lbs, 220.462, 0.01);
    ASSERT_NEAR(sh_lbs_to_kg(lbs), kg, 0.0001);
}

TEST(unit_price_conversion)
{
    /* $3.00/gallon -> $0.79/liter (approx) */
    double ppg = 3.00;
    double ppl = sh_price_per_gallon_to_liter(ppg);
    ASSERT_NEAR(ppl, 0.7925, 0.001);

    /* Round-trip */
    ASSERT_NEAR(sh_price_per_liter_to_gallon(ppl), ppg, 0.0001);
}

TEST(unit_parse_metric)
{
    ASSERT_EQ(sh_parse_units("metric"), SH_UNITS_METRIC);
    ASSERT_EQ(sh_parse_units("METRIC"), SH_UNITS_METRIC);
    ASSERT_EQ(sh_parse_units("\"metric\""), SH_UNITS_METRIC);
    ASSERT_EQ(sh_parse_units(NULL), SH_UNITS_METRIC);
    ASSERT_EQ(sh_parse_units("unknown"), SH_UNITS_METRIC);
    ASSERT_EQ(sh_parse_units(""), SH_UNITS_METRIC);
}

TEST(unit_parse_imperial)
{
    ASSERT_EQ(sh_parse_units("imperial"), SH_UNITS_IMPERIAL);
    ASSERT_EQ(sh_parse_units("IMPERIAL"), SH_UNITS_IMPERIAL);
    ASSERT_EQ(sh_parse_units("Imperial"), SH_UNITS_IMPERIAL);
    ASSERT_EQ(sh_parse_units("\"imperial\""), SH_UNITS_IMPERIAL);
    ASSERT_EQ(sh_parse_units("  imperial"), SH_UNITS_IMPERIAL);
}

TEST(unit_string_representation)
{
    ASSERT(strcmp(sh_units_string(SH_UNITS_METRIC), "metric") == 0);
    ASSERT(strcmp(sh_units_string(SH_UNITS_IMPERIAL), "imperial") == 0);
}

TEST(unit_meters_feet)
{
    /* 1 meter = 3.28084 feet */
    ASSERT_NEAR(sh_m_to_ft(1.0), 3.28084, 0.0001);
    /* Round-trip */
    double m = 10.0;
    double ft = sh_m_to_ft(m);
    ASSERT_NEAR(sh_ft_to_m(ft), m, 0.0001);
}

TEST(unit_meters_yards)
{
    /* 1 meter = 1.09361 yards */
    ASSERT_NEAR(sh_m_to_yards(1.0), 1.09361, 0.0001);
    /* Round-trip */
    double m = 100.0;
    ASSERT_NEAR(sh_yards_to_m(sh_m_to_yards(m)), m, 0.0001);
}

TEST(unit_cm_inches)
{
    /* 2.54 cm = 1 inch exactly */
    ASSERT_NEAR(sh_in_to_cm(1.0), 2.54, 0.0001);
    ASSERT_NEAR(sh_cm_to_in(2.54), 1.0, 0.0001);
    /* Round-trip */
    double cm = 50.0;
    ASSERT_NEAR(sh_in_to_cm(sh_cm_to_in(cm)), cm, 0.0001);
}

TEST(unit_mm_inches)
{
    /* 25.4 mm = 1 inch */
    ASSERT_NEAR(sh_in_to_mm(1.0), 25.4, 0.0001);
    ASSERT_NEAR(sh_mm_to_in(25.4), 1.0, 0.0001);
}

TEST(unit_ml_floz)
{
    /* 29.5735 ml = 1 fl oz */
    ASSERT_NEAR(sh_floz_to_ml(1.0), 29.5735, 0.001);
    /* Round-trip */
    double ml = 500.0;
    ASSERT_NEAR(sh_floz_to_ml(sh_ml_to_floz(ml)), ml, 0.001);
}

TEST(unit_grams_ounces)
{
    /* 28.3495 g = 1 oz */
    ASSERT_NEAR(sh_oz_to_g(1.0), 28.3495, 0.001);
    /* Round-trip */
    double g = 100.0;
    ASSERT_NEAR(sh_oz_to_g(sh_g_to_oz(g)), g, 0.001);
}

TEST(unit_tonnes)
{
    /* 1000 kg = 1 metric tonne */
    ASSERT_NEAR(sh_kg_to_tonnes(1000.0), 1.0, 0.0001);
    ASSERT_NEAR(sh_tonnes_to_kg(1.0), 1000.0, 0.0001);
    /* 907.18474 kg = 1 US short ton */
    ASSERT_NEAR(sh_kg_to_tons_us(907.18474), 1.0, 0.0001);
    ASSERT_NEAR(sh_tons_us_to_kg(1.0), 907.18474, 0.001);
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

    printf("\nArena Allocator:\n");
    RUN_TEST(arena_create_free);
    RUN_TEST(arena_alloc_basic);
    RUN_TEST(arena_calloc_zeroed);
    RUN_TEST(arena_reset);
    RUN_TEST(arena_overflow_returns_null);
    RUN_TEST(arena_alignment);

    printf("\nMemory Pool:\n");
    RUN_TEST(pool_init_free);
    RUN_TEST(pool_alloc_basic);
    RUN_TEST(pool_ptr_access);
    RUN_TEST(pool_reset);
    RUN_TEST(pool_overflow_returns_invalid);
    RUN_TEST(pool_grow);

    printf("\nRate Limiter:\n");
    RUN_TEST(ratelimit_create_free);
    RUN_TEST(ratelimit_create_invalid_params);
    RUN_TEST(ratelimit_free_null_safe);
    RUN_TEST(ratelimit_check_allows_within_burst);
    RUN_TEST(ratelimit_check_denies_over_burst);
    RUN_TEST(ratelimit_different_ips_independent);
    RUN_TEST(ratelimit_ipv6_support);
    RUN_TEST(ratelimit_ipv4_ipv6_different_buckets);
    RUN_TEST(ratelimit_stats);
    RUN_TEST(ratelimit_reset);
    RUN_TEST(ratelimit_zero_addr_allowed);
    RUN_TEST(ratelimit_null_safe);

    printf("\nWork Queue:\n");
    RUN_TEST(workqueue_create_free);
    RUN_TEST(workqueue_create_invalid);
    RUN_TEST(workqueue_free_null_safe);
    RUN_TEST(workqueue_push_pop_basic);
    RUN_TEST(workqueue_full_returns_zero);
    RUN_TEST(workqueue_item_expiration);
    RUN_TEST(workqueue_stats);
    RUN_TEST(workqueue_try_push_pressure);
    RUN_TEST(workqueue_shutdown);
    RUN_TEST(workqueue_null_safety);
    RUN_TEST(workqueue_fifo_order);
    RUN_TEST(workqueue_item_cancel_basic);
    RUN_TEST(workqueue_item_cancel_null_queue);
    RUN_TEST(workqueue_item_cancel_null_safety);
    RUN_TEST(workqueue_item_cancel_multiple_times);

    printf("\nCompletion Signaling:\n");
    RUN_TEST(completion_init_cleanup);
    RUN_TEST(completion_init_null_safe);
    RUN_TEST(completion_signal_immediate);
    RUN_TEST(completion_cancel);
    RUN_TEST(completion_cancel_null_safe);
    RUN_TEST(completion_signal_null_safe);
    RUN_TEST(completion_wait_success);
    RUN_TEST(completion_wait_timeout);

    printf("\nWorker Pool:\n");
    RUN_TEST(worker_pool_create_free);
    RUN_TEST(worker_pool_create_invalid);
    RUN_TEST(worker_pool_null_safe);
    RUN_TEST(worker_pool_auto_detect);
    RUN_TEST(worker_pool_processes_items);

    printf("\nCapacity Planning:\n");
    RUN_TEST(capacity_calculate_basic);
    RUN_TEST(capacity_calculate_invalid_input);
    RUN_TEST(capacity_calculate_defaults);
    RUN_TEST(capacity_calculate_high_load);
    RUN_TEST(capacity_report);
    RUN_TEST(capacity_report_null_safe);
    RUN_TEST(capacity_validate_good_config);
    RUN_TEST(capacity_validate_queue_too_deep);
    RUN_TEST(capacity_validate_timeout_too_short);
    RUN_TEST(capacity_validate_null_safe);

    printf("\nAdaptive Capacity:\n");
    RUN_TEST(adaptive_create_free);
    RUN_TEST(adaptive_create_defaults);
    RUN_TEST(adaptive_free_null_safe);
    RUN_TEST(adaptive_record_basic);
    RUN_TEST(adaptive_percentiles);
    RUN_TEST(adaptive_recalculate);
    RUN_TEST(adaptive_update_interval);
    RUN_TEST(adaptive_sample_cap);
    RUN_TEST(adaptive_get_params);
    RUN_TEST(adaptive_null_safety);

    printf("\nArgs Parsing:\n");
    RUN_TEST(args_init);
    RUN_TEST(args_parse_basic);
    RUN_TEST(args_parse_rate_limit);
    RUN_TEST(args_parse_rate_limit_off);
    RUN_TEST(args_parse_adaptive);
    RUN_TEST(args_parse_positional);
    RUN_TEST(args_prefix);

    printf("\nCircuit Breaker:\n");
    RUN_TEST(circuit_create_free);
    RUN_TEST(circuit_create_with_config);
    RUN_TEST(circuit_free_null_safe);
    RUN_TEST(circuit_allow_closed);
    RUN_TEST(circuit_opens_on_failures);
    RUN_TEST(circuit_success_resets_failures);
    RUN_TEST(circuit_half_open_recovers);
    RUN_TEST(circuit_half_open_failure_reopens);
    RUN_TEST(circuit_stats);
    RUN_TEST(circuit_reset);

    printf("\nExponential Backoff:\n");
    RUN_TEST(backoff_init);
    RUN_TEST(backoff_init_with_config);
    RUN_TEST(backoff_exponential);
    RUN_TEST(backoff_max_cap);
    RUN_TEST(backoff_reset);
    RUN_TEST(backoff_calculate_stateless);
    RUN_TEST(backoff_jitter_range);

    printf("\nHTTP Retry:\n");
    RUN_TEST(retry_init);
    RUN_TEST(retry_success_no_retry);
    RUN_TEST(retry_retryable_status);
    RUN_TEST(retry_on_503);
    RUN_TEST(retry_honors_retry_after);
    RUN_TEST(retry_caps_retry_after);
    RUN_TEST(retry_exhausts_retries);
    RUN_TEST(retry_with_circuit);

    printf("\nCORS:\n");
    RUN_TEST(cors_init);
    RUN_TEST(cors_add_origin);
    RUN_TEST(cors_is_allowed_all);
    RUN_TEST(cors_is_allowed_whitelist);
    RUN_TEST(cors_headers_wildcard);
    RUN_TEST(cors_headers_specific);
    RUN_TEST(cors_preflight_headers);
    RUN_TEST(cors_parse_origins);
    RUN_TEST(cors_credentials);

    printf("\nLogging:\n");
    RUN_TEST(log_init_shutdown);
    RUN_TEST(log_level_from_string);
    RUN_TEST(log_level_to_string);
    RUN_TEST(log_set_level);
    RUN_TEST(log_enabled);
    RUN_TEST(log_trace_id);
    RUN_TEST(log_format_from_string);

    printf("\nTrace IDs:\n");
    RUN_TEST(trace_generate);
    RUN_TEST(trace_validate);
    RUN_TEST(trace_set_get);
    RUN_TEST(trace_new);
    RUN_TEST(trace_format_header);
    RUN_TEST(trace_span_basic);

    printf("\nMetrics:\n");
    RUN_TEST(metrics_init_shutdown);
    RUN_TEST(metrics_counter);
    RUN_TEST(metrics_gauge);
    RUN_TEST(metrics_histogram);
    RUN_TEST(metrics_timer);
    RUN_TEST(metrics_prometheus_output);
    RUN_TEST(metrics_with_tags);
    RUN_TEST(metrics_http_request);
    RUN_TEST(metrics_statsd_hostname_resolution);

    printf("\nHashmap:\n");
    RUN_TEST(hashmap_i64_create_free);
    RUN_TEST(hashmap_i64_insert_lookup);
    RUN_TEST(hashmap_i64_update);
    RUN_TEST(hashmap_i64_contains);
    RUN_TEST(hashmap_i64_zero_key_rejected);
    RUN_TEST(hashmap_i64_clear);
    RUN_TEST(hashmap_i64_resize);
    RUN_TEST(hashmap_i64_iterator);
    RUN_TEST(hashmap_i64u32_basic);
    RUN_TEST(hashmap_null_safety);

    printf("\nHeap:\n");
    RUN_TEST(heap_create_free);
    RUN_TEST(heap_push_pop);
    RUN_TEST(heap_peek);
    RUN_TEST(heap_contains_priority);
    RUN_TEST(heap_decrease_key);
    RUN_TEST(heap_decrease_key_noop);
    RUN_TEST(heap_push_or_decrease);
    RUN_TEST(heap_clear);
    RUN_TEST(heap_many_entries);
    RUN_TEST(heap_null_safety);

    printf("\nSpatial Grid:\n");
    RUN_TEST(dynamic_grid_create_free);
    RUN_TEST(dynamic_grid_insert_query);
    RUN_TEST(dynamic_grid_radius_query);
    RUN_TEST(dynamic_grid_out_of_bounds);
    RUN_TEST(dynamic_grid_duplicate_detection);
    RUN_TEST(csr_grid_create_free);
    RUN_TEST(csr_grid_query_cell);
    RUN_TEST(csr_grid_find_nearest);
    RUN_TEST(csr_grid_cell_index);
    RUN_TEST(grid_config_create);
    RUN_TEST(spatial_grid_null_safety);

    printf("\nQuery String Parsing:\n");
    RUN_TEST(query_get_int_basic);
    RUN_TEST(query_get_int_default);
    RUN_TEST(query_get_int_edge_cases);
    RUN_TEST(query_get_str_basic);
    RUN_TEST(query_get_str_not_found);
    RUN_TEST(query_get_str_null_safety);
    RUN_TEST(query_has_basic);
    RUN_TEST(query_has_null_safety);
    RUN_TEST(query_get_double_basic);
    RUN_TEST(query_get_double_default);

    printf("\nRender Utilities:\n");
    RUN_TEST(render_set_get_pixel);
    RUN_TEST(render_get_pixel_out_of_bounds);
    RUN_TEST(render_set_pixel_out_of_bounds);
    RUN_TEST(render_blend_pixel_opaque);
    RUN_TEST(render_blend_pixel_transparent);
    RUN_TEST(render_blend_pixel_50_percent);
    RUN_TEST(render_fill_span_opaque);
    RUN_TEST(render_fill_span_clipping);
    RUN_TEST(render_fill_span_out_of_bounds_y);
    RUN_TEST(render_fill_span_alpha_blend);
    RUN_TEST(render_clear_buffer);
    RUN_TEST(render_clear_buffer_uniform);
    RUN_TEST(render_clear_rect);
    RUN_TEST(render_clear_rect_clipping);
    RUN_TEST(render_color_macros);

    printf("\nUnit Conversions:\n");
    RUN_TEST(unit_km_miles_roundtrip);
    RUN_TEST(unit_miles_km_roundtrip);
    RUN_TEST(unit_meters_km);
    RUN_TEST(unit_meters_miles);
    RUN_TEST(unit_efficiency_conversion);
    RUN_TEST(unit_efficiency_zero_handling);
    RUN_TEST(unit_volume_conversion);
    RUN_TEST(unit_weight_conversion);
    RUN_TEST(unit_price_conversion);
    RUN_TEST(unit_parse_metric);
    RUN_TEST(unit_parse_imperial);
    RUN_TEST(unit_string_representation);
    RUN_TEST(unit_meters_feet);
    RUN_TEST(unit_meters_yards);
    RUN_TEST(unit_cm_inches);
    RUN_TEST(unit_mm_inches);
    RUN_TEST(unit_ml_floz);
    RUN_TEST(unit_grams_ounces);
    RUN_TEST(unit_tonnes);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
