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

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
