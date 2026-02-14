/*
 * test_eov.c - Tests for Hungarian EOV ↔ WGS84 conversion
 *
 * Reference points verified via round-trip testing and cross-checked
 * against known Hungarian city coordinates.
 */

#include "sh_eov.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

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

#define ASSERT_NEAR(a, b, eps) do { \
    double _a = (a), _b = (b), _eps = (eps); \
    if (fabs(_a - _b) > _eps) { \
        printf("[FAIL]\n    Expected: %.8f\n    Got:      %.8f (diff=%.8f, eps=%.8f)\n    at %s:%d\n", \
               _b, _a, fabs(_a - _b), _eps, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

/*
 * Tolerance: Bursa-Wolf 7-parameter transform has ~1-2m accuracy.
 * 0.0001° ≈ 11m, sufficient for our purposes.
 */
#define WGS84_TOL 0.0001
#define EOV_TOL   2.0  /* 2 meters */

/*
 * Reference correspondences (WGS84 → EOV, verified via round-trip):
 *   Budapest (47.486, 19.043) ↔ EOV(649664, 238006)
 *   Debrecen (47.531, 21.627) ↔ EOV(844219, 246232)
 *   Pécs     (46.072, 18.233) ↔ EOV(586993, 81150)
 *   Sopron   (47.685, 16.590) ↔ EOV(465516, 263059)
 */

/* Budapest, Gellért Hill */
TEST(eov_to_wgs84_budapest)
{
    double lat, lon;
    int rc = sh_eov_to_wgs84(649664, 238006, &lat, &lon);
    ASSERT(rc == 0);
    ASSERT_NEAR(lat, 47.486, WGS84_TOL);
    ASSERT_NEAR(lon, 19.043, WGS84_TOL);
}

/* Debrecen (eastern Hungary) */
TEST(eov_to_wgs84_debrecen)
{
    double lat, lon;
    int rc = sh_eov_to_wgs84(844219, 246232, &lat, &lon);
    ASSERT(rc == 0);
    ASSERT_NEAR(lat, 47.531, WGS84_TOL);
    ASSERT_NEAR(lon, 21.627, WGS84_TOL);
}

/* Pécs (southern Hungary) */
TEST(eov_to_wgs84_pecs)
{
    double lat, lon;
    int rc = sh_eov_to_wgs84(586993, 81150, &lat, &lon);
    ASSERT(rc == 0);
    ASSERT_NEAR(lat, 46.072, WGS84_TOL);
    ASSERT_NEAR(lon, 18.233, WGS84_TOL);
}

/* Sopron (western Hungary, near Austrian border) */
TEST(eov_to_wgs84_sopron)
{
    double lat, lon;
    int rc = sh_eov_to_wgs84(465516, 263059, &lat, &lon);
    ASSERT(rc == 0);
    ASSERT_NEAR(lat, 47.685, WGS84_TOL);
    ASSERT_NEAR(lon, 16.590, WGS84_TOL);
}

/* EOV origin: false easting=650000, false northing=200000
 * Should map to approximately the central meridian */
TEST(eov_to_wgs84_origin)
{
    double lat, lon;
    int rc = sh_eov_to_wgs84(650000.0, 200000.0, &lat, &lon);
    ASSERT(rc == 0);
    /* Origin should be near the central meridian 19.0486° and lat of origin 47.144° */
    ASSERT_NEAR(lon, 19.049, 0.01);
    ASSERT_NEAR(lat, 47.144, 0.01);
}

/* Round-trip: EOV → WGS84 → EOV */
TEST(eov_roundtrip_budapest)
{
    double lat, lon;
    sh_eov_to_wgs84(649664, 238006, &lat, &lon);

    double y_out, x_out;
    int rc = sh_wgs84_to_eov(lat, lon, &y_out, &x_out);
    ASSERT(rc == 0);
    ASSERT_NEAR(y_out, 649664.0, EOV_TOL);
    ASSERT_NEAR(x_out, 238006.0, EOV_TOL);
}

TEST(eov_roundtrip_debrecen)
{
    double lat, lon;
    sh_eov_to_wgs84(844219, 246232, &lat, &lon);

    double y_out, x_out;
    int rc = sh_wgs84_to_eov(lat, lon, &y_out, &x_out);
    ASSERT(rc == 0);
    ASSERT_NEAR(y_out, 844219.0, EOV_TOL);
    ASSERT_NEAR(x_out, 246232.0, EOV_TOL);
}

TEST(eov_roundtrip_pecs)
{
    double lat, lon;
    sh_eov_to_wgs84(586993, 81150, &lat, &lon);

    double y_out, x_out;
    int rc = sh_wgs84_to_eov(lat, lon, &y_out, &x_out);
    ASSERT(rc == 0);
    ASSERT_NEAR(y_out, 586993.0, EOV_TOL);
    ASSERT_NEAR(x_out, 81150.0, EOV_TOL);
}

/* WGS84 → EOV → WGS84 round-trip */
TEST(wgs84_roundtrip)
{
    double eov_y, eov_x;
    int rc = sh_wgs84_to_eov(47.4979, 19.0402, &eov_y, &eov_x);
    ASSERT(rc == 0);

    double lat, lon;
    rc = sh_eov_to_wgs84(eov_y, eov_x, &lat, &lon);
    ASSERT(rc == 0);
    ASSERT_NEAR(lat, 47.4979, WGS84_TOL);
    ASSERT_NEAR(lon, 19.0402, WGS84_TOL);
}

/* Error cases */
TEST(eov_null_output)
{
    ASSERT(sh_eov_to_wgs84(650000, 238000, NULL, NULL) == -1);
}

TEST(eov_out_of_range)
{
    double lat, lon;
    ASSERT(sh_eov_to_wgs84(0, 0, &lat, &lon) == -1);
    ASSERT(sh_eov_to_wgs84(1000000, 500000, &lat, &lon) == -1);
}

TEST(wgs84_out_of_range)
{
    double y, x;
    ASSERT(sh_wgs84_to_eov(0, 0, &y, &x) == -1);
    ASSERT(sh_wgs84_to_eov(51.5, -0.1, &y, &x) == -1); /* London */
}

TEST(wgs84_null_output)
{
    ASSERT(sh_wgs84_to_eov(47.5, 19.0, NULL, NULL) == -1);
}

int main(void)
{
    printf("\nEOV ↔ WGS84 Conversion Tests:\n");

    printf("\n  EOV → WGS84:\n");
    RUN_TEST(eov_to_wgs84_budapest);
    RUN_TEST(eov_to_wgs84_debrecen);
    RUN_TEST(eov_to_wgs84_pecs);
    RUN_TEST(eov_to_wgs84_sopron);
    RUN_TEST(eov_to_wgs84_origin);

    printf("\n  Round-trip:\n");
    RUN_TEST(eov_roundtrip_budapest);
    RUN_TEST(eov_roundtrip_debrecen);
    RUN_TEST(eov_roundtrip_pecs);
    RUN_TEST(wgs84_roundtrip);

    printf("\n  Error handling:\n");
    RUN_TEST(eov_null_output);
    RUN_TEST(eov_out_of_range);
    RUN_TEST(wgs84_out_of_range);
    RUN_TEST(wgs84_null_output);

    printf("\nEOV: %d passed, %d total\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
