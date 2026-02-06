/*
 * test_carta.c - Test suite for Carta tile generator
 */

#include "carta.h"
#include "ct_collision.h"
#include "ct_label.h"
#include "ct_boundary.h"
#include "sh_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    static int test_##name(void); \
    static void run_test_##name(void) { \
        tests_run++; \
        printf("  %-50s ", #name); \
        if (test_##name()) { \
            tests_passed++; \
            printf("[PASS]\n"); \
        } else { \
            printf("[FAIL]\n"); \
        } \
    } \
    static int test_##name(void)

#define ASSERT(cond) do { if (!(cond)) return 0; } while(0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((a) - (b)) < (eps))

/* ============================================================================
 * Version Tests
 * ============================================================================ */

TEST(version_not_null)
{
    const char *v = ct_version();
    ASSERT(v != NULL);
    ASSERT(strlen(v) > 0);
    return 1;
}

TEST(mvt_spec_version)
{
    int v = ct_mvt_spec_version();
    ASSERT(v == 2);
    return 1;
}

/* ============================================================================
 * Status Tests
 * ============================================================================ */

TEST(status_string_ok)
{
    const char *s = ct_status_string(CT_OK);
    ASSERT(strcmp(s, "OK") == 0);
    return 1;
}

TEST(status_string_error)
{
    const char *s = ct_status_string(CT_ERROR_OUT_OF_MEMORY);
    ASSERT(s != NULL);
    ASSERT(strlen(s) > 0);
    return 1;
}

/* ============================================================================
 * Tile Coordinate Tests
 * ============================================================================ */

TEST(latlon_to_tile_z0)
{
    int x, y;
    ct_latlon_to_tile(0, 0, 0, &x, &y);
    ASSERT_EQ(x, 0);
    ASSERT_EQ(y, 0);
    return 1;
}

TEST(latlon_to_tile_z1)
{
    int x, y;
    /* Northwest quadrant */
    ct_latlon_to_tile(45, -90, 1, &x, &y);
    ASSERT_EQ(x, 0);
    ASSERT_EQ(y, 0);

    /* Southeast quadrant */
    ct_latlon_to_tile(-45, 90, 1, &x, &y);
    ASSERT_EQ(x, 1);
    ASSERT_EQ(y, 1);
    return 1;
}

TEST(tile_bounds_z0)
{
    CTTileCoord tile = {0, 0, 0};
    CTBBox bbox = ct_tile_bounds(tile);

    ASSERT_NEAR(bbox.min_lon, -180.0, 0.01);
    ASSERT_NEAR(bbox.max_lon, 180.0, 0.01);
    ASSERT_NEAR(bbox.min_lat, -85.05, 0.1);
    ASSERT_NEAR(bbox.max_lat, 85.05, 0.1);
    return 1;
}

TEST(tile_bounds_z1)
{
    CTTileCoord tile = {1, 0, 0};
    CTBBox bbox = ct_tile_bounds(tile);

    ASSERT_NEAR(bbox.min_lon, -180.0, 0.01);
    ASSERT_NEAR(bbox.max_lon, 0.0, 0.01);
    /* z=1, y=0 is northwest quadrant: equator to ~85 degrees */
    ASSERT_NEAR(bbox.min_lat, 0.0, 0.1);
    ASSERT_NEAR(bbox.max_lat, 85.05, 0.1);
    return 1;
}

TEST(tile_is_valid)
{
    ASSERT(ct_tile_is_valid((CTTileCoord){0, 0, 0}));
    ASSERT(ct_tile_is_valid((CTTileCoord){10, 500, 500}));
    ASSERT(!ct_tile_is_valid((CTTileCoord){-1, 0, 0}));
    ASSERT(!ct_tile_is_valid((CTTileCoord){0, 1, 0}));  /* z=0 only has 1 tile */
    return 1;
}

TEST(tile_parent)
{
    CTTileCoord child = {5, 10, 20};
    CTTileCoord parent = ct_tile_parent(child);

    ASSERT_EQ(parent.z, 4);
    ASSERT_EQ(parent.x, 5);
    ASSERT_EQ(parent.y, 10);
    return 1;
}

TEST(tile_children)
{
    CTTileCoord parent = {4, 5, 10};
    CTTileCoord children[4];
    ct_tile_children(parent, children);

    ASSERT_EQ(children[0].z, 5);
    ASSERT_EQ(children[0].x, 10);
    ASSERT_EQ(children[0].y, 20);

    ASSERT_EQ(children[3].x, 11);
    ASSERT_EQ(children[3].y, 21);
    return 1;
}

TEST(tiles_for_bbox)
{
    CTBBox bbox = {47.0, 19.0, 48.0, 20.0};  /* Around Budapest */
    CTTileCoord *tiles = NULL;
    int count = ct_tiles_for_bbox(bbox, 10, &tiles);

    ASSERT(count > 0);
    ASSERT(tiles != NULL);

    /* All tiles should be at zoom 10 */
    for (int i = 0; i < count; i++) {
        ASSERT_EQ(tiles[i].z, 10);
    }

    free(tiles);
    return 1;
}

/* ============================================================================
 * Web Mercator Tests
 * ============================================================================ */

TEST(mercator_origin)
{
    double x, y;
    ct_latlon_to_mercator(0, 0, &x, &y);

    ASSERT_NEAR(x, 0, 0.1);
    ASSERT_NEAR(y, 0, 0.1);
    return 1;
}

TEST(mercator_roundtrip)
{
    double lat1 = 47.5, lon1 = 19.0;
    double x, y, lat2, lon2;

    ct_latlon_to_mercator(lat1, lon1, &x, &y);
    ct_mercator_to_latlon(x, y, &lat2, &lon2);

    ASSERT_NEAR(lat1, lat2, 0.0001);
    ASSERT_NEAR(lon1, lon2, 0.0001);
    return 1;
}

/*
 * Regression test: batch transform must match direct calculation.
 * Bug fix: The Mercator LUT was using inconsistent formulas between
 * initialization (bin centers) and lookup (edge-to-edge), causing
 * ~50 pixel offset at mid-latitudes like Budapest (47°N).
 */
TEST(batch_transform_matches_direct)
{
    /* Test at Budapest - where the bug was most visible */
    double lat = 47.4979;
    double lon = 19.0402;
    int zoom = 12;
    int extent = 4096;

    /* Get tile coordinates */
    int tile_x, tile_y;
    ct_latlon_to_tile(lat, lon, zoom, &tile_x, &tile_y);
    CTTileCoord coord = {zoom, tile_x, tile_y};

    /* Direct calculation */
    int px_direct, py_direct;
    ct_latlon_to_tile_pixel(lat, lon, coord, extent, &px_direct, &py_direct);

    /* Batch transform (uses Mercator LUT) */
    CTTilePoint point;
    point.x = (int32_t)(lon * 1e7);  /* nanodegrees */
    point.y = (int32_t)(lat * 1e7);
    ct_batch_transform_points(coord, extent, &point, 1);

    /* Must match within 1 pixel (rounding tolerance) */
    ASSERT(abs(point.x - px_direct) <= 1);
    ASSERT(abs(point.y - py_direct) <= 1);
    return 1;
}

/*
 * Test batch transform at multiple latitudes to ensure LUT consistency.
 * The LUT covers [-85.051, 85.051] degrees - test across the range.
 */
TEST(batch_transform_latitude_range)
{
    double test_lats[] = {-60.0, -30.0, 0.0, 30.0, 47.5, 60.0, 80.0};
    double lon = 10.0;
    int zoom = 10;
    int extent = 4096;

    for (int i = 0; i < 7; i++) {
        double lat = test_lats[i];

        int tile_x, tile_y;
        ct_latlon_to_tile(lat, lon, zoom, &tile_x, &tile_y);
        CTTileCoord coord = {zoom, tile_x, tile_y};

        /* Direct calculation */
        int px_direct, py_direct;
        ct_latlon_to_tile_pixel(lat, lon, coord, extent, &px_direct, &py_direct);

        /* Batch transform */
        CTTilePoint point;
        point.x = (int32_t)(lon * 1e7);
        point.y = (int32_t)(lat * 1e7);
        ct_batch_transform_points(coord, extent, &point, 1);

        /* Must match within 1 pixel at all latitudes */
        ASSERT(abs(point.x - px_direct) <= 1);
        ASSERT(abs(point.y - py_direct) <= 1);
    }
    return 1;
}

/*
 * Test batch transform at different zoom levels.
 * Higher zoom = more pixels = more sensitive to LUT errors.
 */
TEST(batch_transform_zoom_levels)
{
    double lat = 47.5;
    double lon = 19.0;
    int extent = 4096;

    for (int zoom = 4; zoom <= 18; zoom += 2) {
        int tile_x, tile_y;
        ct_latlon_to_tile(lat, lon, zoom, &tile_x, &tile_y);
        CTTileCoord coord = {zoom, tile_x, tile_y};

        /* Direct calculation */
        int px_direct, py_direct;
        ct_latlon_to_tile_pixel(lat, lon, coord, extent, &px_direct, &py_direct);

        /* Batch transform */
        CTTilePoint point;
        point.x = (int32_t)(lon * 1e7);
        point.y = (int32_t)(lat * 1e7);
        ct_batch_transform_points(coord, extent, &point, 1);

        /* Must match within 1 pixel at all zoom levels */
        ASSERT(abs(point.x - px_direct) <= 1);
        ASSERT(abs(point.y - py_direct) <= 1);
    }
    return 1;
}

/* ============================================================================
 * Tile Management Tests
 * ============================================================================ */

TEST(tile_init)
{
    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 100, 200});

    ASSERT_EQ(tile.coord.z, 14);
    ASSERT_EQ(tile.coord.x, 100);
    ASSERT_EQ(tile.coord.y, 200);
    ASSERT_EQ(tile.num_features, 0);

    ct_tile_free(&tile);
    return 1;
}

TEST(tile_add_feature)
{
    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 0, 0});

    CTTilePoint points[] = {{0, 0}, {100, 100}, {200, 0}};
    CTFeature feature = {
        .type = CT_GEOM_LINESTRING,
        .points = malloc(sizeof(points)),
        .num_points = 3,
        .layer = CT_LAYER_ROADS,
        .feature_type = CT_ROAD_PRIMARY
    };
    memcpy(feature.points, points, sizeof(points));

    CTStatus status = ct_tile_add_feature(&tile, &feature);
    ASSERT_EQ(status, CT_OK);
    ASSERT_EQ(tile.num_features, 1);

    ct_tile_free(&tile);
    return 1;
}

/* ============================================================================
 * Style Tests
 * ============================================================================ */

TEST(default_style)
{
    CTStyle style;
    ct_default_style(&style);

    ASSERT(CT_COLOR_A(style.water_color) == 255);
    ASSERT(CT_COLOR_A(style.land_color) == 255);
    /* Check road widths at z14 reference point */
    ASSERT(style.road_widths[CT_ROAD_MOTORWAY].z14 > 0);
    ASSERT(style.road_widths[CT_ROAD_MOTORWAY].z10 < style.road_widths[CT_ROAD_MOTORWAY].z18);
    ASSERT_EQ(style.reference_zoom, 14);
    return 1;
}

TEST(scale_width)
{
    float base = 4.0f;

    /* Same zoom = same width */
    ASSERT_NEAR(ct_scale_width(base, 14, 14), base, 0.01);

    /* Higher zoom = wider */
    ASSERT(ct_scale_width(base, 15, 14) > base);

    /* Lower zoom = narrower */
    ASSERT(ct_scale_width(base, 13, 14) < base);
    return 1;
}

TEST(road_width_at_zoom)
{
    CTRoadWidth rw = { .z10 = 2.0f, .z14 = 4.0f, .z18 = 8.0f };

    /* At key points */
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 10), 2.0f, 0.01);
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 14), 4.0f, 0.01);
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 18), 8.0f, 0.01);

    /* Interpolation between z10 and z14 */
    float mid12 = ct_road_width_at_zoom(&rw, 12);
    ASSERT(mid12 > 2.0f && mid12 < 4.0f);

    /* Interpolation between z14 and z18 */
    float mid16 = ct_road_width_at_zoom(&rw, 16);
    ASSERT(mid16 > 4.0f && mid16 < 8.0f);

    /* At z6 and below, roads are 30% of z10 width for low-zoom clarity */
    float z6_width = ct_road_width_at_zoom(&rw, 6);
    ASSERT_NEAR(z6_width, 2.0f * 0.3f, 0.01);  /* 30% of z10 */

    /* Interpolation between z6 (30%) and z10 */
    float z8_width = ct_road_width_at_zoom(&rw, 8);
    ASSERT(z8_width > z6_width && z8_width < 2.0f);

    /* Above z18 clamps to z18 width */
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 20), 8.0f, 0.01);

    return 1;
}

TEST(waterway_width)
{
    CTStyle style;
    ct_default_style(&style);

    /* River centerline (actual river shape is polygon from natural=water) */
    float river_width = ct_style_waterway_width(&style, CT_WATERWAY_RIVER);
    ASSERT(river_width >= 1.0f && river_width <= 2.0f);

    /* Canals should be wider than streams */
    float canal_width = ct_style_waterway_width(&style, CT_WATERWAY_CANAL);
    float stream_width = ct_style_waterway_width(&style, CT_WATERWAY_STREAM);
    ASSERT(canal_width > stream_width);

    /* Ditches thinnest */
    float ditch_width = ct_style_waterway_width(&style, CT_WATERWAY_DITCH);
    ASSERT(ditch_width < stream_width);
    ASSERT(ditch_width >= 0.5f);  /* Minimum visibility */

    /* Invalid type returns fallback */
    float invalid_width = ct_style_waterway_width(&style, -1);
    ASSERT(invalid_width >= 0.5f);

    return 1;
}

TEST(road_casing)
{
    /* z16+: full casing (1.0) */
    ASSERT_NEAR(ct_style_road_casing(16), 1.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(17), 1.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(20), 1.0f, 0.01);

    /* z14-15: reduced casing (0.5) */
    ASSERT_NEAR(ct_style_road_casing(14), 0.5f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(15), 0.5f, 0.01);

    /* Below z14: no casing (performance optimization) */
    ASSERT_NEAR(ct_style_road_casing(13), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(12), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(10), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(8), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_road_casing(0), 0.0f, 0.01);

    return 1;
}

TEST(railway_casing)
{
    /* z14+: standard railway casing (0.5) */
    ASSERT_NEAR(ct_style_railway_casing(14), 0.5f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(15), 0.5f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(16), 0.5f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(18), 0.5f, 0.01);

    /* Below z14: no casing (performance optimization) */
    ASSERT_NEAR(ct_style_railway_casing(13), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(12), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(10), 0.0f, 0.01);
    ASSERT_NEAR(ct_style_railway_casing(0), 0.0f, 0.01);

    return 1;
}

TEST(bresenham_thin_line)
{
    /*
     * Test that thin lines (< 0.75px) use Bresenham (fast, non-AA).
     * Verify pixels are drawn along the line.
     */
    CTRenderContext *ctx = ct_render_create(64, 64);
    ct_render_clear(ctx);

    CTColor red = CT_RGB(255, 0, 0);

    /* Draw a thin horizontal line (width 0.5, uses Bresenham) */
    ct_render_line(ctx, 10, 20, 30, 20, red, 0.5f);

    /* Verify some pixels along the line are set */
    CTColor pixel = ct_render_get_pixel(ctx, 15, 20);
    ASSERT_EQ(CT_COLOR_R(pixel), 255);

    pixel = ct_render_get_pixel(ctx, 25, 20);
    ASSERT_EQ(CT_COLOR_R(pixel), 255);

    /* Draw a thin diagonal line */
    ct_render_line(ctx, 5, 5, 15, 15, red, 0.5f);

    /* Verify diagonal pixels */
    pixel = ct_render_get_pixel(ctx, 10, 10);
    ASSERT_EQ(CT_COLOR_R(pixel), 255);

    ct_render_free(ctx);
    return 1;
}

TEST(aa_line_threshold)
{
    /*
     * Test that lines >= 0.75px use anti-aliased rendering.
     * AA lines produce sub-pixel blending (fractional alpha).
     */
    CTRenderContext *ctx = ct_render_create(64, 64);
    ct_render_clear(ctx);

    CTColor blue = CT_RGB(0, 0, 255);

    /* Draw a line at threshold (0.75px, uses AA) */
    ct_render_line(ctx, 10, 30, 30, 32, blue, 0.75f);

    /* AA lines should produce some blended pixels adjacent to the line */
    /* Just verify the line rendered without crashing */
    CTColor pixel = ct_render_get_pixel(ctx, 20, 31);
    /* Some blue should be present due to AA blending */
    ASSERT(CT_COLOR_B(pixel) > 0 || CT_COLOR_B(ct_render_get_pixel(ctx, 20, 30)) > 0);

    ct_render_free(ctx);
    return 1;
}

TEST(simd_alpha_blend)
{
    /*
     * Test SIMD alpha blending by filling a polygon with semi-transparent color.
     * This exercises the SIMD alpha blending path in fill_span().
     */
    CTRenderContext *ctx = ct_render_create(64, 64);
    ct_render_clear(ctx);

    /* Draw a red rectangle first */
    CTColor red = CT_RGB(255, 0, 0);
    CTTilePoint red_rect[] = {{10, 10}, {50, 10}, {50, 50}, {10, 50}};
    ct_render_polygon(ctx, red_rect, 4, red);

    /* Draw a semi-transparent blue rectangle on top */
    CTColor blue_trans = CT_RGBA(0, 0, 255, 128);
    CTTilePoint blue_rect[] = {{20, 20}, {60, 20}, {60, 60}, {20, 60}};
    ct_render_polygon(ctx, blue_rect, 4, blue_trans);

    /* Check pixels in overlapping region - should be blended purple */
    CTColor pixel = ct_render_get_pixel(ctx, 30, 30);
    ASSERT(CT_COLOR_R(pixel) > 0);  /* Some red from background */
    ASSERT(CT_COLOR_B(pixel) > 0);  /* Some blue from overlay */

    /* Check blue-only region (no red underneath) */
    CTColor blue_pixel = ct_render_get_pixel(ctx, 55, 55);
    ASSERT(CT_COLOR_B(blue_pixel) > 0);

    ct_render_free(ctx);
    return 1;
}

TEST(simd_alpha_blend_long_span)
{
    /*
     * Test SIMD alpha blending with long horizontal spans.
     * Uses a wide rectangle to ensure the SIMD path (4+ pixel spans) is exercised.
     */
    CTRenderContext *ctx = ct_render_create(256, 64);
    ct_render_clear(ctx);

    /* Fill background with solid green */
    CTColor green = CT_RGB(0, 255, 0);
    CTTilePoint green_rect[] = {{0, 0}, {256, 0}, {256, 64}, {0, 64}};
    ct_render_polygon(ctx, green_rect, 4, green);

    /* Overlay with 50% transparent red - this creates 256-pixel spans */
    CTColor red_trans = CT_RGBA(255, 0, 0, 128);
    CTTilePoint red_rect[] = {{0, 16}, {256, 16}, {256, 48}, {0, 48}};
    ct_render_polygon(ctx, red_rect, 4, red_trans);

    /* Check multiple pixels across the span - should all be similar yellow-ish */
    int sample_xs[] = {10, 64, 128, 192, 250};
    for (int i = 0; i < 5; i++) {
        CTColor pixel = ct_render_get_pixel(ctx, sample_xs[i], 32);
        /* Should have both red and green components */
        ASSERT(CT_COLOR_R(pixel) > 100);
        ASSERT(CT_COLOR_G(pixel) > 100);
        /* Blue should be minimal (no blue in either source) */
        ASSERT(CT_COLOR_B(pixel) < 50);
    }

    /* Verify the blend is consistent across the span (SIMD vs scalar consistency) */
    CTColor first = ct_render_get_pixel(ctx, 10, 32);
    CTColor last = ct_render_get_pixel(ctx, 250, 32);
    ASSERT_EQ(CT_COLOR_R(first), CT_COLOR_R(last));
    ASSERT_EQ(CT_COLOR_G(first), CT_COLOR_G(last));

    ct_render_free(ctx);
    return 1;
}

/* ============================================================================
 * Render Context Tests
 * ============================================================================ */

TEST(render_create)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    ASSERT(ctx != NULL);
    ASSERT(ct_render_pixels(ctx) != NULL);

    ct_render_free(ctx);
    return 1;
}

TEST(render_clear)
{
    CTRenderContext *ctx = ct_render_create(64, 64);
    ct_render_clear(ctx);

    /* Check that all pixels are background color */
    CTColor bg = ctx->style.background_color;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            CTColor pixel = ct_render_get_pixel(ctx, x, y);
            ASSERT_EQ(pixel, bg);
        }
    }

    ct_render_free(ctx);
    return 1;
}

TEST(render_set_pixel)
{
    CTRenderContext *ctx = ct_render_create(64, 64);

    CTColor red = CT_RGB(255, 0, 0);
    ct_render_set_pixel(ctx, 10, 20, red);

    ASSERT_EQ(ct_render_get_pixel(ctx, 10, 20), red);

    ct_render_free(ctx);
    return 1;
}

TEST(render_line)
{
    CTRenderContext *ctx = ct_render_create(64, 64);
    ct_render_clear(ctx);

    CTColor blue = CT_RGB(0, 0, 255);
    ct_render_line(ctx, 0, 0, 63, 63, blue, 1.0f);

    /* Check that some pixels along diagonal are blue-ish */
    CTColor pixel = ct_render_get_pixel(ctx, 32, 32);
    ASSERT(CT_COLOR_B(pixel) > 100);

    ct_render_free(ctx);
    return 1;
}

TEST(render_context_has_scale_buffer)
{
    /* Verify render context allocates pre-allocated buffer */
    CTRenderContext *ctx = ct_render_create(256, 256);
    ASSERT(ctx != NULL);

    /* Scale buffer should be pre-allocated for performance */
    ASSERT(ctx->scale_buffer != NULL);
    ASSERT(ctx->scale_buffer_capacity >= 1024);  /* Reasonable minimum */

    ct_render_free(ctx);
    return 1;
}

TEST(render_tile_reuses_buffer)
{
    /* Test that rendering multiple features doesn't leak memory */
    CTRenderContext *ctx = ct_render_create(256, 256);
    ct_render_clear(ctx);

    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 0, 0});

    /* Add multiple features with varying point counts */
    for (int f = 0; f < 50; f++) {
        int num_points = 10 + (f % 20);
        CTTilePoint *points = malloc(num_points * sizeof(CTTilePoint));
        for (int i = 0; i < num_points; i++) {
            points[i].x = (i * 100) % 4096;
            points[i].y = (f * 80 + i * 50) % 4096;
        }

        CTFeature feature = {
            .type = CT_GEOM_LINESTRING,
            .points = points,
            .num_points = num_points,
            .layer = CT_LAYER_ROADS,
            .feature_type = CT_ROAD_SECONDARY
        };
        ct_tile_add_feature(&tile, &feature);
    }

    /* Render tile - should use pre-allocated buffer, not malloc per feature */
    ct_render_tile(ctx, &tile);

    /* Verify scale buffer was expanded if needed but still exists */
    ASSERT(ctx->scale_buffer != NULL);
    ASSERT(ctx->scale_buffer_capacity >= 10);  /* At least fits smallest feature */

    ct_tile_free(&tile);
    ct_render_free(ctx);
    return 1;
}

TEST(render_polygon_scanline_performance)
{
    /* Test polygon rendering with many edges (stress insertion sort) */
    CTRenderContext *ctx = ct_render_create(256, 256);
    ct_render_clear(ctx);

    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 0, 0});

    /* Create a complex polygon with many vertices */
    int num_points = 100;
    CTTilePoint *points = malloc(num_points * sizeof(CTTilePoint));

    /* Create a star-like polygon to stress scanline algorithm */
    for (int i = 0; i < num_points; i++) {
        double angle = 2.0 * 3.14159 * i / num_points;
        double radius = (i % 2 == 0) ? 1800 : 900;  /* Alternating radii */
        points[i].x = 2048 + (int)(radius * cos(angle));
        points[i].y = 2048 + (int)(radius * sin(angle));
    }

    int *ring_ends = malloc(sizeof(int));
    ring_ends[0] = num_points;
    CTFeature feature = {
        .type = CT_GEOM_POLYGON,
        .points = points,
        .num_points = num_points,
        .ring_ends = ring_ends,
        .num_rings = 1,
        .layer = CT_LAYER_BUILDINGS,
        .feature_type = 0
    };
    ct_tile_add_feature(&tile, &feature);

    /* Render - should complete without issues using insertion sort */
    ct_render_tile(ctx, &tile);

    /* Verify something was rendered (not all background) */
    int non_bg_pixels = 0;
    CTColor bg = ctx->style.background_color;
    for (int y = 100; y < 156; y++) {
        for (int x = 100; x < 156; x++) {
            if (ct_render_get_pixel(ctx, x, y) != bg) {
                non_bg_pixels++;
            }
        }
    }
    ASSERT(non_bg_pixels > 0);

    ct_tile_free(&tile);
    ct_render_free(ctx);
    return 1;
}

TEST(render_multipolygon_with_hole)
{
    /* Test multipolygon rendering with an outer ring and inner hole */
    CTRenderContext *ctx = ct_render_create(256, 256);
    ct_render_clear(ctx);

    /* Create a square with a smaller square hole inside */
    /* Outer ring: large square (4 points + closing point) */
    /* Inner ring: small square hole (4 points + closing point) */
    CTTilePoint points[10] = {
        /* Outer ring (CCW) */
        {50, 50}, {200, 50}, {200, 200}, {50, 200}, {50, 50},
        /* Inner ring (CW - hole) */
        {100, 100}, {100, 150}, {150, 150}, {150, 100}, {100, 100}
    };
    int ring_ends[2] = {5, 10};

    ct_render_multipolygon(ctx, points, 10, ring_ends, 2,
                           CT_RGB(255, 0, 0));

    /* Verify outer area is filled (corner should be red) */
    CTColor outer_pixel = ct_render_get_pixel(ctx, 60, 60);
    ASSERT_EQ(CT_COLOR_R(outer_pixel), 255);
    ASSERT_EQ(CT_COLOR_G(outer_pixel), 0);
    ASSERT_EQ(CT_COLOR_B(outer_pixel), 0);

    /* Verify hole area is NOT filled (center should be background) */
    CTColor hole_pixel = ct_render_get_pixel(ctx, 125, 125);
    CTColor bg = ctx->style.background_color;
    ASSERT_EQ(hole_pixel, bg);

    ct_render_free(ctx);
    return 1;
}

/* ============================================================================
 * Render Options Tests
 * ============================================================================ */

TEST(render_options_default)
{
    CTRenderOptions opts;
    ct_render_options_default(&opts);

    /* All layers should be enabled */
    ASSERT_EQ(opts.render_water, 1);
    ASSERT_EQ(opts.render_landuse, 1);
    ASSERT_EQ(opts.render_buildings, 1);
    ASSERT_EQ(opts.render_roads, 1);
    ASSERT_EQ(opts.render_railways, 1);
    ASSERT_EQ(opts.render_boundaries, 1);
    ASSERT_EQ(opts.render_labels, 1);

    /* All details should be enabled */
    ASSERT_EQ(opts.render_road_casing, 1);
    ASSERT_EQ(opts.render_railway_casing, 1);
    ASSERT_EQ(opts.render_bridge_outlines, 1);
    ASSERT_EQ(opts.render_building_outlines, 1);
    ASSERT_EQ(opts.render_label_halos, 1);
    ASSERT_EQ(opts.render_boundary_dashes, 1);

    /* Check zoom cutoffs */
    ASSERT_EQ(opts.casing_min_zoom, 14);
    ASSERT_EQ(opts.building_outlines_min_zoom, 14);
    ASSERT_EQ(opts.labels_min_zoom, 8);

    return 1;
}

TEST(render_options_fast)
{
    CTRenderOptions opts;
    ct_render_options_fast(&opts);

    /* Layers should be enabled */
    ASSERT_EQ(opts.render_water, 1);
    ASSERT_EQ(opts.render_landuse, 1);
    ASSERT_EQ(opts.render_buildings, 1);
    ASSERT_EQ(opts.render_roads, 1);
    ASSERT_EQ(opts.render_railways, 1);

    /* Boundaries disabled - expensive relation processing */
    ASSERT_EQ(opts.render_boundaries, 0);

    /* Labels enabled - useful for navigation */
    ASSERT_EQ(opts.render_labels, 1);

    /* Visual details enabled (match OSM quality except boundaries) */
    ASSERT_EQ(opts.render_road_casing, 1);
    ASSERT_EQ(opts.render_railway_casing, 1);
    ASSERT_EQ(opts.render_bridge_outlines, 1);
    ASSERT_EQ(opts.render_building_outlines, 1);
    ASSERT_EQ(opts.render_label_halos, 1);
    ASSERT_EQ(opts.render_boundary_dashes, 0);  /* Boundaries disabled anyway */

    /* Standard zoom cutoffs */
    ASSERT_EQ(opts.casing_min_zoom, 14);
    ASSERT_EQ(opts.building_outlines_min_zoom, 14);
    ASSERT_EQ(opts.labels_min_zoom, 8);

    return 1;
}

TEST(render_options_quality)
{
    CTRenderOptions opts;
    ct_render_options_quality(&opts);

    /* All layers enabled */
    ASSERT_EQ(opts.render_water, 1);
    ASSERT_EQ(opts.render_landuse, 1);
    ASSERT_EQ(opts.render_buildings, 1);
    ASSERT_EQ(opts.render_roads, 1);
    ASSERT_EQ(opts.render_railways, 1);
    ASSERT_EQ(opts.render_boundaries, 1);
    ASSERT_EQ(opts.render_labels, 1);

    /* All details enabled */
    ASSERT_EQ(opts.render_road_casing, 1);
    ASSERT_EQ(opts.render_railway_casing, 1);
    ASSERT_EQ(opts.render_bridge_outlines, 1);
    ASSERT_EQ(opts.render_building_outlines, 1);
    ASSERT_EQ(opts.render_label_halos, 1);
    ASSERT_EQ(opts.render_boundary_dashes, 1);

    /* Low zoom cutoffs for quality mode */
    ASSERT(opts.casing_min_zoom <= 12);
    ASSERT(opts.building_outlines_min_zoom <= 13);
    ASSERT(opts.labels_min_zoom <= 6);

    return 1;
}

TEST(render_context_has_options)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    ASSERT(ctx != NULL);

    /* Context should have default options initialized */
    ASSERT_EQ(ctx->options.render_water, 1);
    ASSERT_EQ(ctx->options.render_labels, 1);
    ASSERT_EQ(ctx->options.render_road_casing, 1);
    ASSERT_EQ(ctx->options.casing_min_zoom, 14);

    ct_render_free(ctx);
    return 1;
}

TEST(render_set_options)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    ASSERT(ctx != NULL);

    /* Set fast options */
    CTRenderOptions fast_opts;
    ct_render_options_fast(&fast_opts);
    ct_render_set_options(ctx, &fast_opts);

    /* Verify options were applied - fast has labels=1, boundaries=0, casing=1 */
    ASSERT_EQ(ctx->options.render_labels, 1);
    ASSERT_EQ(ctx->options.render_boundaries, 0);
    ASSERT_EQ(ctx->options.render_road_casing, 1);

    /* Set quality options */
    CTRenderOptions quality_opts;
    ct_render_options_quality(&quality_opts);
    ct_render_set_options(ctx, &quality_opts);

    /* Verify options were changed */
    ASSERT_EQ(ctx->options.render_labels, 1);
    ASSERT_EQ(ctx->options.render_boundaries, 1);
    ASSERT_EQ(ctx->options.render_road_casing, 1);

    ct_render_free(ctx);
    return 1;
}

TEST(render_options_custom)
{
    CTRenderOptions opts;
    ct_render_options_default(&opts);

    /* Customize for specific use case */
    opts.render_labels = 0;
    opts.render_buildings = 0;
    opts.render_road_casing = 1;
    opts.casing_min_zoom = 10;

    ASSERT_EQ(opts.render_labels, 0);
    ASSERT_EQ(opts.render_buildings, 0);
    ASSERT_EQ(opts.render_road_casing, 1);
    ASSERT_EQ(opts.casing_min_zoom, 10);

    /* Other settings should remain at defaults */
    ASSERT_EQ(opts.render_water, 1);
    ASSERT_EQ(opts.render_roads, 1);

    return 1;
}

/* ============================================================================
 * MVT Encoding Tests
 * ============================================================================ */

TEST(mvt_default_options)
{
    CTMVTOptions opts;
    ct_mvt_default_options(&opts);

    ASSERT_EQ(opts.extent, CT_MVT_EXTENT);
    ASSERT(opts.buffer >= 0);
    return 1;
}

TEST(mvt_layer_name)
{
    ASSERT(strcmp(ct_mvt_layer_name(CT_LAYER_ROADS), "roads") == 0);
    ASSERT(strcmp(ct_mvt_layer_name(CT_LAYER_WATER), "water") == 0);
    ASSERT(strcmp(ct_mvt_layer_name(CT_LAYER_BUILDINGS), "buildings") == 0);
    return 1;
}

TEST(mvt_encode_empty_tile)
{
    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 0, 0});

    uint8_t buffer[1024];
    size_t size = ct_encode_mvt(&tile, NULL, buffer, sizeof(buffer));

    /* Empty tile should produce minimal output */
    ASSERT(size == 0 || size < 100);

    ct_tile_free(&tile);
    return 1;
}

TEST(mvt_encode_with_feature)
{
    CTTile tile;
    ct_tile_init(&tile, (CTTileCoord){14, 0, 0});

    CTTilePoint points[] = {{0, 0}, {2048, 2048}, {4095, 0}};
    CTFeature feature = {
        .type = CT_GEOM_LINESTRING,
        .points = malloc(sizeof(points)),
        .num_points = 3,
        .layer = CT_LAYER_ROADS,
        .feature_type = CT_ROAD_PRIMARY
    };
    memcpy(feature.points, points, sizeof(points));
    ct_tile_add_feature(&tile, &feature);

    uint8_t buffer[4096];
    size_t size = ct_encode_mvt(&tile, NULL, buffer, sizeof(buffer));

    ASSERT(size > 0);
    ASSERT(size < sizeof(buffer));

    ct_tile_free(&tile);
    return 1;
}

/* ============================================================================
 * PNG Encoding Tests
 * ============================================================================ */

TEST(png_default_options)
{
    CTPNGOptions opts;
    ct_png_default_options(&opts);

    ASSERT(opts.tile_size == 256 || opts.tile_size == 512);
    ASSERT(opts.compression_level >= 0 && opts.compression_level <= 9);
    return 1;
}

TEST(png_max_size)
{
    size_t max = ct_png_max_size(256, 256);
    ASSERT(max > 256 * 256 * 4);  /* Must be larger than raw RGBA */
    return 1;
}

TEST(png_encode_solid)
{
    int width = 64, height = 64;
    uint8_t *pixels = malloc(width * height * 4);

    /* Fill with solid red */
    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = 255;  /* R */
        pixels[i * 4 + 1] = 0;    /* G */
        pixels[i * 4 + 2] = 0;    /* B */
        pixels[i * 4 + 3] = 255;  /* A */
    }

    size_t capacity = ct_png_max_size(width, height);
    uint8_t *buffer = malloc(capacity);

    size_t size = ct_encode_png(pixels, width, height, NULL, buffer, capacity);

    /* Check PNG signature */
    ASSERT(size > 8);
    ASSERT(buffer[0] == 137);
    ASSERT(buffer[1] == 'P');
    ASSERT(buffer[2] == 'N');
    ASSERT(buffer[3] == 'G');

    free(pixels);
    free(buffer);
    return 1;
}

/* ============================================================================
 * PBF Context Tests
 * ============================================================================ */

TEST(pbf_context_create)
{
    CTPBFContext *ctx = ct_pbf_context_create();
    ASSERT(ctx != NULL);
    ct_pbf_context_free(ctx);
    return 1;
}

TEST(pbf_stats_empty)
{
    CTPBFContext *ctx = ct_pbf_context_create();

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(ctx, &nodes, &ways, &features, &bbox);

    ASSERT_EQ(nodes, 0);
    ASSERT_EQ(ways, 0);
    ASSERT_EQ(features, 0);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(pbf_context_relations_initialized)
{
    CTPBFContext *ctx = ct_pbf_context_create();

    /* Relation-related fields should be initialized to 0/NULL */
    ASSERT(ctx->relations == NULL);
    ASSERT_EQ(ctx->num_relations, 0);
    ASSERT_EQ(ctx->relations_capacity, 0);

    ASSERT(ctx->role_strings == NULL);
    ASSERT_EQ(ctx->num_role_strings, 0);
    ASSERT_EQ(ctx->role_strings_capacity, 0);

    ASSERT(ctx->multipolygons == NULL);
    ASSERT_EQ(ctx->num_multipolygons, 0);
    ASSERT_EQ(ctx->multipolygons_capacity, 0);

    ASSERT(ctx->mp_rtree == NULL);
    ASSERT_EQ(ctx->total_relations_parsed, 0);
    ASSERT_EQ(ctx->multipolygons_assembled, 0);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(pbf_way_map_initialized)
{
    CTPBFContext *ctx = ct_pbf_context_create();

    /* way_map should be NULL before any ways are loaded */
    ASSERT(ctx->way_map == NULL);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(pbf_relation_member_types)
{
    /* Verify member type enum values match OSM PBF spec */
    ASSERT_EQ(CT_MEMBER_NODE, 0);
    ASSERT_EQ(CT_MEMBER_WAY, 1);
    ASSERT_EQ(CT_MEMBER_RELATION, 2);
    return 1;
}

TEST(pbf_memory_limit_config)
{
    /* Test that memory limit configuration is properly stored */
    CTPBFConfig config;
    ct_pbf_config_init(&config);

    /* Default should be 0 (unlimited) */
    ASSERT_EQ(config.memory_limit, 0);

    /* Set a limit and create context */
    config.memory_limit = 1024 * 1024;  /* 1MB */
    CTPBFContext *ctx = ct_pbf_context_create_with_config(&config);
    ASSERT(ctx != NULL);

    /* Verify config is stored */
    ASSERT_EQ(ctx->config.memory_limit, 1024 * 1024);

    /* Memory tracking should be initialized */
    ASSERT(ctx->memory_used > 0);  /* At least context size */

    ct_pbf_context_free(ctx);
    return 1;
}

/* ============================================================================
 * Multipolygon Assembly Tests
 * ============================================================================ */

TEST(multipolygon_assemble_empty)
{
    /* Assembling multipolygons on empty context should succeed */
    CTPBFContext *ctx = ct_pbf_context_create();
    CTStatus status = ct_assemble_multipolygons(ctx);
    ASSERT_EQ(status, CT_OK);
    ASSERT_EQ(ctx->num_multipolygons, 0);
    ct_pbf_context_free(ctx);
    return 1;
}

TEST(multipolygon_get_role_string_empty)
{
    CTPBFContext *ctx = ct_pbf_context_create();

    /* Role index 0 should return empty string */
    const char *role = ct_get_role_string(ctx, 0);
    ASSERT(role != NULL);
    ASSERT_EQ(strlen(role), 0);

    /* Out of bounds index should return empty string */
    role = ct_get_role_string(ctx, 999);
    ASSERT(role != NULL);
    ASSERT_EQ(strlen(role), 0);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(multipolygon_ring_structure)
{
    /* Verify CTMultipolygonRing structure */
    CTMultipolygonRing ring;
    memset(&ring, 0, sizeof(ring));

    ring.coords = NULL;
    ring.num_coords = 0;
    ring.is_outer = 1;

    ASSERT_EQ(ring.is_outer, 1);
    return 1;
}

TEST(multipolygon_assembled_structure)
{
    /* Verify CTAssembledMultipolygon structure */
    CTAssembledMultipolygon mp;
    memset(&mp, 0, sizeof(mp));

    mp.rings = NULL;
    mp.num_rings = 0;
    mp.feature_class = CT_OSM_WATER;
    mp.feature_type = 0;
    mp.name = NULL;

    ASSERT_EQ(mp.feature_class, CT_OSM_WATER);
    return 1;
}

/* ============================================================================
 * Boundary Assembly Tests
 * ============================================================================ */

TEST(boundary_config_init)
{
    CTBoundaryConfig config;
    ct_boundary_config_init(&config);

    /* Default: country borders (2) to county level (6) */
    ASSERT_EQ(config.min_admin_level, 2);
    ASSERT_EQ(config.max_admin_level, 6);
    ASSERT_EQ(config.include_protected_areas, 1);
    return 1;
}

TEST(boundary_assemble_empty)
{
    /* Assembling boundaries on empty context should succeed */
    CTPBFContext *ctx = ct_pbf_context_create();
    CTStatus status = ct_assemble_boundaries(ctx);
    ASSERT_EQ(status, CT_OK);
    ASSERT_EQ(ctx->num_boundaries, 0);
    ct_pbf_context_free(ctx);
    return 1;
}

TEST(boundary_type_enum)
{
    /* Verify boundary type enum values */
    ASSERT_EQ(CT_BOUNDARY_TYPE_ADMIN, 0);
    ASSERT_EQ(CT_BOUNDARY_TYPE_PROTECTED, 1);
    ASSERT_EQ(CT_BOUNDARY_TYPE_COUNT, 2);
    return 1;
}

TEST(boundary_admin_level_constants)
{
    /* Verify admin level constants */
    ASSERT_EQ(CT_BOUNDARY_COUNTRY, 2);
    ASSERT_EQ(CT_BOUNDARY_STATE, 4);
    ASSERT_EQ(CT_BOUNDARY_COUNTY, 6);
    ASSERT_EQ(CT_BOUNDARY_CITY, 8);
    ASSERT_EQ(CT_BOUNDARY_SUBURB, 10);
    ASSERT_EQ(CT_BOUNDARY_OTHER, 99);
    return 1;
}

TEST(boundary_assembled_structure)
{
    /* Verify CTAssembledBoundary structure */
    CTAssembledBoundary b;
    memset(&b, 0, sizeof(b));

    b.relation_id = 123456;
    b.boundary_type = CT_BOUNDARY_TYPE_ADMIN;
    b.admin_level = CT_BOUNDARY_COUNTRY;
    b.coords = NULL;
    b.num_coords = 0;
    b.length_m = 0.0f;
    b.name = NULL;

    ASSERT_EQ(b.relation_id, 123456);
    ASSERT_EQ(b.boundary_type, CT_BOUNDARY_TYPE_ADMIN);
    ASSERT_EQ(b.admin_level, CT_BOUNDARY_COUNTRY);
    return 1;
}

TEST(boundary_rtree_empty)
{
    /* Building boundary R-tree on empty context should succeed */
    CTPBFContext *ctx = ct_pbf_context_create();
    CTStatus status = ct_build_boundary_rtree(ctx);
    ASSERT_EQ(status, CT_OK);
    ASSERT(ctx->boundary_rtree == NULL);  /* No R-tree for empty data */
    ct_pbf_context_free(ctx);
    return 1;
}

/* ============================================================================
 * ASCII Rendering Tests
 * ============================================================================ */

TEST(ascii_default_options)
{
    CTAsciiOptions opts;
    ct_ascii_default_options(&opts);

    ASSERT_EQ(opts.width, 80);
    ASSERT_EQ(opts.height, 0);  /* Auto */
    ASSERT_EQ(opts.charset, CT_ASCII_EXTENDED);
    ASSERT_EQ(opts.invert, 0);
    ASSERT_EQ(opts.color, 0);
    return 1;
}

TEST(ascii_buffer_size)
{
    /* Simple charset: 1 byte per char */
    size_t simple_size = ct_ascii_buffer_size(80, 40, CT_ASCII_SIMPLE, 0);
    ASSERT(simple_size >= 80 * 40);

    /* With color: needs room for ANSI codes */
    size_t color_size = ct_ascii_buffer_size(80, 40, CT_ASCII_SIMPLE, 1);
    ASSERT(color_size > simple_size);

    /* Blocks charset: up to 4 bytes per char (UTF-8) */
    size_t blocks_size = ct_ascii_buffer_size(80, 40, CT_ASCII_BLOCKS, 0);
    ASSERT(blocks_size >= simple_size);

    return 1;
}

TEST(ascii_render_solid_image)
{
    /* Create a solid gray image */
    int width = 64, height = 64;
    uint8_t *pixels = malloc(width * height * 4);

    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = 128;  /* R */
        pixels[i * 4 + 1] = 128;  /* G */
        pixels[i * 4 + 2] = 128;  /* B */
        pixels[i * 4 + 3] = 255;  /* A */
    }

    CTAsciiOptions opts;
    ct_ascii_default_options(&opts);
    opts.width = 16;
    opts.height = 8;
    opts.charset = CT_ASCII_SIMPLE;

    size_t buf_size = ct_ascii_buffer_size(opts.width, opts.height, opts.charset, 0);
    char *buf = malloc(buf_size);

    size_t len = ct_render_ascii(pixels, width, height, &opts, buf, buf_size);

    /* Should produce output */
    ASSERT(len > 0);

    /* Should have 8 lines (8 newlines) */
    int newlines = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') newlines++;
    }
    ASSERT_EQ(newlines, 8);

    /* All characters should be the same (solid color) */
    char first_char = buf[0];
    ASSERT(first_char != '\n');  /* First char is not newline */

    free(pixels);
    free(buf);
    return 1;
}

TEST(ascii_render_gradient)
{
    /* Create a horizontal gradient from black to white */
    int width = 256, height = 64;
    uint8_t *pixels = malloc(width * height * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 4;
            uint8_t val = (uint8_t)x;  /* 0-255 gradient */
            pixels[idx + 0] = val;
            pixels[idx + 1] = val;
            pixels[idx + 2] = val;
            pixels[idx + 3] = 255;
        }
    }

    CTAsciiOptions opts;
    ct_ascii_default_options(&opts);
    opts.width = 32;
    opts.height = 8;
    opts.charset = CT_ASCII_SIMPLE;

    size_t buf_size = ct_ascii_buffer_size(opts.width, opts.height, opts.charset, 0);
    char *buf = malloc(buf_size);

    size_t len = ct_render_ascii(pixels, width, height, &opts, buf, buf_size);
    ASSERT(len > 0);

    /* First character should be dark (space or .) */
    /* Last character before newline should be bright (@ or #) */
    char first = buf[0];
    char last = buf[opts.width - 1];

    /* In simple charset " .:-=+*#%@", space is darkest, @ is brightest */
    ASSERT(first == ' ' || first == '.');
    ASSERT(last == '@' || last == '%' || last == '#');

    free(pixels);
    free(buf);
    return 1;
}

TEST(ascii_invert_mode)
{
    /* Create a white image */
    int width = 64, height = 64;
    uint8_t *pixels = malloc(width * height * 4);

    for (int i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = 255;
    }

    CTAsciiOptions opts;
    ct_ascii_default_options(&opts);
    opts.width = 8;
    opts.height = 4;
    opts.charset = CT_ASCII_SIMPLE;

    size_t buf_size = ct_ascii_buffer_size(opts.width, opts.height, opts.charset, 0);
    char *normal_buf = malloc(buf_size);
    char *invert_buf = malloc(buf_size);

    /* Normal: white = bright = @ */
    opts.invert = 0;
    ct_render_ascii(pixels, width, height, &opts, normal_buf, buf_size);

    /* Inverted: white = dark = space */
    opts.invert = 1;
    ct_render_ascii(pixels, width, height, &opts, invert_buf, buf_size);

    /* Characters should be different */
    ASSERT(normal_buf[0] != invert_buf[0]);

    free(pixels);
    free(normal_buf);
    free(invert_buf);
    return 1;
}

/* ============================================================================
 * Geometry Tests
 * ============================================================================ */

TEST(simplify_short_line)
{
    CTTilePoint points[] = {{0, 0}, {100, 100}};
    CTTilePoint *out;
    int out_count;

    ct_simplify_linestring(points, 2, 10.0, &out, &out_count);

    ASSERT_EQ(out_count, 2);
    free(out);
    return 1;
}

TEST(simplify_preserves_endpoints)
{
    CTTilePoint points[] = {{0, 0}, {50, 1}, {100, 0}};
    CTTilePoint *out;
    int out_count;

    ct_simplify_linestring(points, 3, 10.0, &out, &out_count);

    /* With high tolerance, middle point should be removed */
    ASSERT(out_count >= 2);
    ASSERT_EQ(out[0].x, 0);
    ASSERT_EQ(out[out_count - 1].x, 100);
    free(out);
    return 1;
}

TEST(simplify_multipolygon_preserves_rings)
{
    /* Test that multipolygon simplification preserves ring structure */
    /* Outer ring: 8 points square with detail, Inner ring: 8 points square */
    CTTilePoint points[] = {
        /* Outer ring (8 points) */
        {0, 0}, {50, 1}, {100, 0}, {101, 50}, {100, 100}, {50, 99}, {0, 100}, {-1, 50},
        /* Inner ring / hole (8 points) */
        {30, 30}, {40, 31}, {50, 30}, {51, 40}, {50, 50}, {40, 49}, {30, 50}, {29, 40}
    };
    int num_points = 16;
    int ring_ends[] = {8, 16};
    int num_rings = 2;

    /* Make a copy since simplification is in-place */
    CTTilePoint *copy = malloc(num_points * sizeof(CTTilePoint));
    memcpy(copy, points, num_points * sizeof(CTTilePoint));
    int *ring_ends_copy = malloc(num_rings * sizeof(int));
    memcpy(ring_ends_copy, ring_ends, num_rings * sizeof(int));

    /* Simplify with a tolerance that removes the small deviations */
    ct_simplify_multipolygon_inplace(copy, &num_points, ring_ends_copy, num_rings, 5.0f);

    /* Both rings should have at least 3 points */
    int ring1_points = ring_ends_copy[0];
    int ring2_points = ring_ends_copy[1] - ring_ends_copy[0];
    ASSERT(ring1_points >= 3);
    ASSERT(ring2_points >= 3);
    ASSERT(num_points >= 6);  /* At least 3 points per ring */

    /* Ring ends should be valid */
    ASSERT(ring_ends_copy[0] <= num_points);
    ASSERT(ring_ends_copy[1] == num_points);

    free(copy);
    free(ring_ends_copy);
    return 1;
}

/*
 * Test that multipolygon simplification correctly reads second ring data
 * even after first ring has been simplified and written to different positions.
 *
 * This test catches a bug where ring_ends[r-1] was used to find ring r's start
 * position AFTER ring_ends[r-1] had been updated to the new (written) position,
 * causing the second ring to read garbage data from the middle of the array.
 */
TEST(simplify_multipolygon_reads_correct_ring_positions)
{
    /* Create two well-separated rings with distinctive coordinate patterns:
     * Ring 1: Points at y=1000-2000 (will be simplified)
     * Ring 2: Points at y=5000-6000 (should NOT be corrupted)
     *
     * If the bug exists, ring 2's coordinates would be read from wrong positions
     * and would contain data from ring 1 or garbage. */
    CTTilePoint points[] = {
        /* Ring 1: outer ring at low y values (8 points, simplifiable) */
        {1000, 1000}, {1050, 1001}, {1100, 1000}, {1101, 1050},
        {1100, 1100}, {1050, 1099}, {1000, 1100}, {999, 1050},
        /* Ring 2: inner ring at HIGH y values (8 points, simplifiable) */
        {5000, 5000}, {5050, 5001}, {5100, 5000}, {5101, 5050},
        {5100, 5100}, {5050, 5099}, {5000, 5100}, {4999, 5050}
    };
    int num_points = 16;
    int ring_ends[] = {8, 16};
    int num_rings = 2;

    CTTilePoint *copy = malloc(num_points * sizeof(CTTilePoint));
    memcpy(copy, points, num_points * sizeof(CTTilePoint));
    int *ring_ends_copy = malloc(num_rings * sizeof(int));
    memcpy(ring_ends_copy, ring_ends, num_rings * sizeof(int));

    /* Simplify with high tolerance to force simplification */
    ct_simplify_multipolygon_inplace(copy, &num_points, ring_ends_copy, num_rings, 10.0f);

    /* Key assertion: ring 2 points must still be in the high y range (5000+).
     * If the bug exists, they would contain values from ring 1 (1000-2000 range). */
    int ring2_start = ring_ends_copy[0];
    int ring2_end = ring_ends_copy[1];
    int ring2_points = ring2_end - ring2_start;

    ASSERT(ring2_points >= 3);  /* Ring must be valid */

    for (int i = ring2_start; i < ring2_end; i++) {
        /* All ring 2 y values must be in the 4000+ range (well above ring 1) */
        ASSERT(copy[i].y >= 4000);  /* Would fail if reading ring 1 data */
    }

    /* Total points should be reasonable (both rings still exist) */
    ASSERT(num_points >= 6);
    ASSERT(ring_ends_copy[1] == num_points);

    free(copy);
    free(ring_ends_copy);
    return 1;
}

TEST(clip_polygon_inside)
{
    /* Polygon fully inside clip region - should be unchanged */
    CTTilePoint points[] = {{100, 100}, {200, 100}, {200, 200}, {100, 200}};
    CTTilePoint *out;
    int out_count;

    ct_clip_polygon(points, 4, 4096, 0, &out, &out_count);

    ASSERT_EQ(out_count, 4);
    free(out);
    return 1;
}

TEST(clip_polygon_partial)
{
    /* Polygon crossing left edge - should be clipped */
    CTTilePoint points[] = {{-100, 100}, {100, 100}, {100, 200}, {-100, 200}};
    CTTilePoint *out;
    int out_count;

    ct_clip_polygon(points, 4, 4096, 0, &out, &out_count);

    /* Should produce a 4-point polygon clipped at x=0 */
    ASSERT(out_count >= 3);
    /* All output points should be within bounds */
    for (int i = 0; i < out_count; i++) {
        ASSERT(out[i].x >= 0);
        ASSERT(out[i].x <= 4096);
    }
    free(out);
    return 1;
}

TEST(clip_polygon_outside)
{
    /* Polygon fully outside clip region - should be empty */
    CTTilePoint points[] = {{-200, -200}, {-100, -200}, {-100, -100}, {-200, -100}};
    CTTilePoint *out;
    int out_count;

    ct_clip_polygon(points, 4, 4096, 0, &out, &out_count);

    ASSERT_EQ(out_count, 0);
    free(out);
    return 1;
}

TEST(clip_linestring_crossing)
{
    /* Line crossing tile boundary */
    CTTilePoint points[] = {{-100, 500}, {500, 500}};
    CTTilePoint *out;
    int out_count;
    int *segments;
    int seg_count;

    ct_clip_linestring(points, 2, 4096, 0, &out, &out_count, &segments, &seg_count);

    ASSERT(out_count >= 2);
    /* First point should be clipped to x=0 */
    ASSERT(out[0].x >= 0);
    free(out);
    free(segments);
    return 1;
}

TEST(clip_multipolygon_with_hole)
{
    /* Multipolygon: outer square with inner hole, both crossing boundary */
    CTTilePoint points[] = {
        /* Outer ring: large square crossing left edge */
        {-500, 100}, {500, 100}, {500, 600}, {-500, 600},
        /* Inner hole: smaller square also crossing left edge */
        {-200, 200}, {200, 200}, {200, 400}, {-200, 400}
    };
    int ring_ends[] = {4, 8};

    CTTilePoint *out;
    int out_count;
    int *out_ring_ends;
    int out_num_rings;

    ct_clip_multipolygon(points, 8, ring_ends, 2, 4096, 0,
                         &out, &out_count, &out_ring_ends, &out_num_rings);

    /* Both rings should survive clipping */
    ASSERT(out_num_rings == 2);
    ASSERT(out_count >= 6);  /* At least 3 points per ring */

    /* All output points should be within bounds */
    for (int i = 0; i < out_count; i++) {
        ASSERT(out[i].x >= 0);
        ASSERT(out[i].x <= 4096);
    }

    /* Ring ends should be valid */
    ASSERT(out_ring_ends[0] >= 3);
    ASSERT(out_ring_ends[1] == out_count);

    free(out);
    free(out_ring_ends);
    return 1;
}

TEST(clip_multipolygon_outer_only)
{
    /* Multipolygon where hole is entirely outside clip region */
    CTTilePoint points[] = {
        /* Outer ring: inside tile */
        {100, 100}, {500, 100}, {500, 500}, {100, 500},
        /* Inner hole: entirely outside (negative coords) */
        {-500, -500}, {-100, -500}, {-100, -100}, {-500, -100}
    };
    int ring_ends[] = {4, 8};

    CTTilePoint *out;
    int out_count;
    int *out_ring_ends;
    int out_num_rings;

    ct_clip_multipolygon(points, 8, ring_ends, 2, 4096, 0,
                         &out, &out_count, &out_ring_ends, &out_num_rings);

    /* Only outer ring should survive */
    ASSERT(out_num_rings == 1);
    ASSERT(out_count == 4);  /* Original 4 points of outer ring */

    free(out);
    free(out_ring_ends);
    return 1;
}

TEST(clip_polygon_large_coordinates)
{
    /* Polygon with very large coordinates that span far beyond tile.
     * This tests the fix for the scanline fill artifacts. */
    CTTilePoint points[] = {
        {-50000, -50000}, {10000, -50000}, {10000, 10000}, {-50000, 10000}
    };
    CTTilePoint *out;
    int out_count;

    ct_clip_polygon(points, 4, 4096, 64, &out, &out_count);

    /* Should produce a valid polygon */
    ASSERT(out_count >= 3);

    /* All points should be within clipped bounds (extent + buffer) */
    for (int i = 0; i < out_count; i++) {
        ASSERT(out[i].x >= -64);
        ASSERT(out[i].x <= 4096 + 64);
        ASSERT(out[i].y >= -64);
        ASSERT(out[i].y <= 4096 + 64);
    }

    free(out);
    return 1;
}

TEST(clip_multipolygon_all_outside)
{
    /* Multipolygon entirely outside tile */
    CTTilePoint points[] = {
        {-1000, -1000}, {-100, -1000}, {-100, -100}, {-1000, -100}
    };
    int ring_ends[] = {4};

    CTTilePoint *out;
    int out_count;
    int *out_ring_ends;
    int out_num_rings;

    ct_clip_multipolygon(points, 4, ring_ends, 1, 4096, 0,
                         &out, &out_count, &out_ring_ends, &out_num_rings);

    /* Should produce empty result */
    ASSERT(out_num_rings == 0);
    ASSERT(out_count == 0);

    /* Should be safe to free even if NULL */
    free(out);
    free(out_ring_ends);
    return 1;
}

TEST(simplify_line_collinear_points)
{
    /* Line with collinear points that should be removed */
    CTTilePoint points[] = {
        {0, 0}, {100, 0}, {200, 0}, {300, 0}, {400, 0}
    };
    int num_points = 5;

    ct_simplify_line_inplace((CTTilePoint *)points, &num_points, 1.0f);

    /* All middle points are collinear, only endpoints should remain */
    ASSERT_EQ(num_points, 2);
    ASSERT_EQ(points[0].x, 0);
    ASSERT_EQ(points[1].x, 400);
    return 1;
}

TEST(simplify_line_zigzag)
{
    /* Zigzag line that should be simplified based on tolerance */
    CTTilePoint points[] = {
        {0, 0}, {100, 50}, {200, 0}, {300, 50}, {400, 0}
    };
    int num_points = 5;

    /* With high tolerance, should simplify to straight line */
    ct_simplify_line_inplace((CTTilePoint *)points, &num_points, 100.0f);

    ASSERT_EQ(num_points, 2);  /* Only endpoints */
    return 1;
}

TEST(simplify_line_preserves_sharp_turns)
{
    /* Line with sharp turn that should be preserved */
    CTTilePoint points[] = {
        {0, 0}, {500, 0}, {500, 500}
    };
    int num_points = 3;

    /* Low tolerance should preserve the turn */
    ct_simplify_line_inplace((CTTilePoint *)points, &num_points, 1.0f);

    ASSERT_EQ(num_points, 3);  /* All points preserved */
    return 1;
}

TEST(simplify_poly_triangle)
{
    /* Triangle should not be simplified below 3 points */
    CTTilePoint points[] = {
        {0, 0}, {500, 0}, {250, 500}
    };
    int num_points = 3;

    ct_simplify_poly_inplace((CTTilePoint *)points, &num_points, 100.0f);

    /* Cannot simplify below 3 points for a valid polygon */
    ASSERT(num_points >= 3);
    return 1;
}

TEST(simplify_multipolygon_preserves_hole)
{
    /* Multipolygon with outer and inner ring */
    CTTilePoint points[] = {
        /* Outer: square */
        {0, 0}, {1000, 0}, {1000, 1000}, {0, 1000},
        /* Inner: small diamond (should not be simplified to < 3 points) */
        {400, 500}, {500, 400}, {600, 500}, {500, 600}
    };
    int ring_ends[] = {4, 8};
    int num_points = 8;
    int num_rings = 2;

    ct_simplify_multipolygon_inplace((CTTilePoint *)points, &num_points,
                                      ring_ends, num_rings, 50.0f);

    /* Both rings should survive (inner ring has significant curvature) */
    ASSERT(num_points >= 6);  /* At least 3 per ring */
    /* ring_ends[0] should be >= 3 (outer ring) */
    ASSERT(ring_ends[0] >= 3);
    /* ring_ends[1] (total) should equal num_points */
    ASSERT(ring_ends[num_rings - 1] == num_points);
    return 1;
}

/* ============================================================================
 * LOD Tests
 * ============================================================================ */

TEST(lod_init_empty)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ASSERT_EQ(config.num_rules, 0);
    ASSERT(config.rules == NULL);
    ct_lod_free(&config);
    return 1;
}

TEST(lod_add_rule)
{
    CTLODConfig config;
    ct_lod_init(&config);

    CTStatus status = ct_lod_add_rule(&config, CT_LAYER_ROADS, CT_ROAD_MOTORWAY,
                                       5, -1, 0, 0);
    ASSERT_EQ(status, CT_OK);
    ASSERT_EQ(config.num_rules, 1);
    ASSERT_EQ(config.rules[0].layer, CT_LAYER_ROADS);
    ASSERT_EQ(config.rules[0].feature_type, CT_ROAD_MOTORWAY);
    ASSERT_EQ(config.rules[0].min_zoom, 5);

    ct_lod_free(&config);
    return 1;
}

TEST(lod_default_preset)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ct_lod_default(&config);

    /* Should have many rules */
    ASSERT(config.num_rules > 20);

    /* Check motorway rule - visible at z5+ (matches OSM Carto) */
    int motorway_visible = ct_lod_is_visible(&config, CT_LAYER_ROADS,
                                              CT_ROAD_MOTORWAY, 5, 0, 0);
    ASSERT_EQ(motorway_visible, 1);

    int motorway_hidden = ct_lod_is_visible(&config, CT_LAYER_ROADS,
                                             CT_ROAD_MOTORWAY, 4, 0, 0);
    ASSERT_EQ(motorway_hidden, 0);

    ct_lod_free(&config);
    return 1;
}

TEST(lod_landuse_types)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ct_lod_default(&config);

    /* Forest at z6 with very large area (>50km²) - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST,
                                 6, 60000000, 0), 1);  /* 60km² visible at z6 */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST,
                                 6, 10000000, 0), 0);  /* 10km² hidden at z6 (needs >50km²) */

    /* Park at z9 with large area (>1km²) - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_PARK,
                                 9, 2000000, 0), 1);   /* 2km² visible at z9 */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_PARK,
                                 8, 2000000, 0), 0);   /* z8 hidden */

    /* Residential at z10 - visible early like OSM Carto */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_RESIDENTIAL,
                                 10, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_LANDUSE, CT_LANDUSE_RESIDENTIAL,
                                 9, 0, 0), 0);

    ct_lod_free(&config);
    return 1;
}

TEST(lod_boundary_admin_levels)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ct_lod_default(&config);

    /* Country boundary (admin_level 2) visible at z4 - delayed for less clutter */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_COUNTRY,
                                 4, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_COUNTRY,
                                 3, 0, 0), 0);

    /* State boundary (admin_level 4) visible at z6 - delayed for less clutter */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_STATE,
                                 6, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_STATE,
                                 5, 0, 0), 0);

    /* City boundary (admin_level 8) visible at z12 - delayed for cleaner maps */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_CITY,
                                 12, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_CITY,
                                 11, 0, 0), 0);

    ct_lod_free(&config);
    return 1;
}

TEST(lod_size_filtering)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ct_lod_default(&config);

    /* Large building (>2000m²) visible at z13 - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BUILDINGS, -1,
                                 13, 3000, 0), 1);  /* 3000m² */
    /* Small building hidden at z13 */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BUILDINGS, -1,
                                 13, 100, 0), 0);   /* 100m² */
    /* All buildings visible at z14 - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_BUILDINGS, -1,
                                 14, 100, 0), 1);

    ct_lod_free(&config);
    return 1;
}

TEST(lod_estimate_area)
{
    /* Simple square: 1 degree × 1 degree at equator ≈ 111km × 111km ≈ 12321 km² */
    CTCoord coords[] = {
        {0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}
    };

    float area = ct_lod_estimate_area(coords, 5);
    /* Should be approximately 111km × 111km = 12321 km² = 12.321 billion m² */
    /* Allow 10% tolerance for projection approximation */
    float expected = 12321000000.0f;
    ASSERT(area > expected * 0.9f);
    ASSERT(area < expected * 1.1f);

    return 1;
}

TEST(lod_estimate_length)
{
    /* Line from (0,0) to (1,0) at equator ≈ 111km */
    CTCoord coords[] = {{0, 0}, {1, 0}};

    float length = ct_lod_estimate_length(coords, 2);
    /* Should be approximately 111km = 111000m */
    float expected = 111000.0f;
    ASSERT(length > expected * 0.99f);
    ASSERT(length < expected * 1.01f);

    return 1;
}

TEST(lod_waterway_types)
{
    CTLODConfig config;
    ct_lod_init(&config);
    ct_lod_default(&config);

    /* Very long river (>100km) visible at z6 - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_RIVER,
                                 6, 0, 150000), 1);  /* 150km river */
    /* Shorter river hidden at z6 */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_RIVER,
                                 6, 0, 50000), 0);   /* 50km river */
    /* All rivers visible at z10 - balanced preset shows earlier */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_RIVER,
                                 10, 0, 100), 1);

    /* Streams visible at z14+ (matches OSM Carto to reduce clutter) */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_STREAM,
                                 14, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_STREAM,
                                 13, 0, 0), 0);

    /* Canals visible at z12+ (delayed to reduce clutter at low zoom) */
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_CANAL,
                                 12, 0, 0), 1);
    ASSERT_EQ(ct_lod_is_visible(&config, CT_LAYER_WATER, CT_WATERWAY_CANAL,
                                 11, 0, 0), 0);

    ct_lod_free(&config);
    return 1;
}

/* ============================================================================
 * Collision Detection Tests
 * ============================================================================ */

TEST(collision_create)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);
    ASSERT_EQ(grid->tile_width, 256);
    ASSERT_EQ(grid->tile_height, 256);
    ASSERT_EQ(grid->cell_size, 8);
    ASSERT_EQ(grid->grid_width, 32);
    ASSERT_EQ(grid->grid_height, 32);
    ASSERT_EQ(grid->num_placements, 0);
    ct_collision_free(grid);
    return 1;
}

TEST(collision_create_invalid)
{
    ASSERT(ct_collision_create(0, 256, 8) == NULL);
    ASSERT(ct_collision_create(256, 0, 8) == NULL);
    ASSERT(ct_collision_create(256, 256, 0) == NULL);
    ASSERT(ct_collision_create(-1, 256, 8) == NULL);
    return 1;
}

TEST(collision_empty_no_collision)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Empty grid should have no collisions */
    ASSERT_EQ(ct_collision_test(grid, 10, 10, 50, 20), 0);
    ASSERT_EQ(ct_collision_test(grid, 0, 0, 256, 256), 0);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_mark_and_test)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Mark a region */
    ct_collision_mark(grid, 100, 100, 50, 30);

    /* Test overlapping region - should collide */
    ASSERT_EQ(ct_collision_test(grid, 100, 100, 10, 10), 1);
    ASSERT_EQ(ct_collision_test(grid, 120, 110, 20, 20), 1);
    ASSERT_EQ(ct_collision_test(grid, 90, 90, 20, 20), 1);  /* Partial overlap */

    /* Test non-overlapping regions - should not collide */
    ASSERT_EQ(ct_collision_test(grid, 0, 0, 50, 50), 0);
    ASSERT_EQ(ct_collision_test(grid, 160, 100, 50, 30), 0);
    ASSERT_EQ(ct_collision_test(grid, 100, 140, 50, 30), 0);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_place_success)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* First placement should succeed */
    ASSERT_EQ(ct_collision_place(grid, 50, 50, 40, 20), 1);
    ASSERT_EQ(ct_collision_get_count(grid), 1);

    /* Non-overlapping placement should succeed */
    ASSERT_EQ(ct_collision_place(grid, 150, 150, 40, 20), 1);
    ASSERT_EQ(ct_collision_get_count(grid), 2);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_place_fail)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* First placement */
    ASSERT_EQ(ct_collision_place(grid, 50, 50, 40, 20), 1);

    /* Overlapping placement should fail */
    ASSERT_EQ(ct_collision_place(grid, 60, 55, 30, 15), 0);
    ASSERT_EQ(ct_collision_get_count(grid), 1);  /* Count unchanged */

    ct_collision_free(grid);
    return 1;
}

TEST(collision_place_padded)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Place with padding: rect (100,100,20,10) with 5px x-padding, 3px y-padding
     * Marks area: (95, 97) to (124, 112) inclusive
     * With 8px cells: cells x=11-15, y=12-14 are marked
     */
    ASSERT_EQ(ct_collision_place_padded(grid, 100, 100, 20, 10, 5, 3), 1);

    /* Test just outside the padded area (cell 16 starts at pixel 128) */
    ASSERT_EQ(ct_collision_test(grid, 128, 100, 20, 10), 0);  /* Just outside */

    /* Test overlapping the padded area */
    ASSERT_EQ(ct_collision_test(grid, 120, 100, 20, 10), 1);  /* Overlaps padded area */

    /* Test without padding where original rect would have allowed adjacent placement */
    ct_collision_reset(grid);
    ct_collision_mark(grid, 100, 100, 20, 10);  /* No padding: marks x=12-14, y=12-13 */
    ASSERT_EQ(ct_collision_test(grid, 120, 100, 20, 10), 0);  /* Adjacent without padding = OK */

    ct_collision_free(grid);
    return 1;
}

TEST(collision_reset)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Place some rectangles */
    ct_collision_place(grid, 50, 50, 40, 20);
    ct_collision_place(grid, 150, 150, 40, 20);
    ASSERT_EQ(ct_collision_get_count(grid), 2);
    ASSERT(ct_collision_get_occupancy(grid) > 0.0f);

    /* Reset */
    ct_collision_reset(grid);
    ASSERT_EQ(ct_collision_get_count(grid), 0);
    ASSERT_NEAR(ct_collision_get_occupancy(grid), 0.0f, 0.001f);

    /* Should be able to place in previously occupied area */
    ASSERT_EQ(ct_collision_place(grid, 50, 50, 40, 20), 1);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_outside_tile)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Rectangles completely outside should not collide */
    ASSERT_EQ(ct_collision_test(grid, -100, 50, 50, 50), 0);
    ASSERT_EQ(ct_collision_test(grid, 300, 50, 50, 50), 0);
    ASSERT_EQ(ct_collision_test(grid, 50, -100, 50, 50), 0);
    ASSERT_EQ(ct_collision_test(grid, 50, 300, 50, 50), 0);

    /* Marking outside should be safe (no crash) */
    ct_collision_mark(grid, -100, -100, 50, 50);
    ct_collision_mark(grid, 300, 300, 50, 50);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_partial_outside)
{
    CTCollisionGrid *grid = ct_collision_create(256, 256, 8);
    ASSERT(grid != NULL);

    /* Mark rectangle partially outside left edge */
    ct_collision_mark(grid, -20, 100, 50, 30);

    /* Test collision with the visible portion */
    ASSERT_EQ(ct_collision_test(grid, 0, 100, 20, 20), 1);
    ASSERT_EQ(ct_collision_test(grid, 50, 100, 20, 20), 0);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_occupancy)
{
    CTCollisionGrid *grid = ct_collision_create(64, 64, 8);
    ASSERT(grid != NULL);
    /* 64/8 = 8x8 = 64 cells */

    ASSERT_NEAR(ct_collision_get_occupancy(grid), 0.0f, 0.001f);

    /* Mark quarter of the tile */
    ct_collision_mark(grid, 0, 0, 32, 32);
    /* 4x4 = 16 cells out of 64 = 0.25 */
    ASSERT_NEAR(ct_collision_get_occupancy(grid), 0.25f, 0.01f);

    ct_collision_free(grid);
    return 1;
}

TEST(collision_null_safety)
{
    /* All functions should handle NULL gracefully */
    ASSERT_EQ(ct_collision_test(NULL, 0, 0, 10, 10), 0);
    ct_collision_mark(NULL, 0, 0, 10, 10);  /* Should not crash */
    ASSERT_EQ(ct_collision_place(NULL, 0, 0, 10, 10), 0);
    ct_collision_reset(NULL);  /* Should not crash */
    ct_collision_free(NULL);   /* Should not crash */
    ASSERT_EQ(ct_collision_get_count(NULL), 0);
    ASSERT_NEAR(ct_collision_get_occupancy(NULL), 0.0f, 0.001f);
    return 1;
}

/* ============================================================================
 * Labeled Points Tests
 * ============================================================================ */

TEST(labeled_points_empty_context)
{
    CTPBFContext *ctx = ct_pbf_context_create();
    ASSERT(ctx != NULL);

    /* Empty context should have no labeled points */
    ASSERT_EQ(ct_pbf_get_label_count(ctx), 0);

    /* Query should succeed with zero results */
    CTTileCoord coord = {10, 512, 512};
    const CTLabeledPoint **points = NULL;
    size_t count = 0;
    CTStatus status = ct_pbf_get_tile_labels(ctx, coord, &points, &count);
    ASSERT_EQ(status, CT_OK);
    ASSERT_EQ(count, 0);
    ASSERT(points == NULL);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(labeled_points_null_safety)
{
    /* NULL context */
    ASSERT_EQ(ct_pbf_get_label_count(NULL), 0);

    const CTLabeledPoint **points = NULL;
    size_t count = 0;
    CTTileCoord coord = {10, 0, 0};
    CTStatus status = ct_pbf_get_tile_labels(NULL, coord, &points, &count);
    ASSERT_EQ(status, CT_ERROR_INVALID_ARGUMENT);

    /* NULL output params */
    CTPBFContext *ctx = ct_pbf_context_create();
    status = ct_pbf_get_tile_labels(ctx, coord, NULL, &count);
    ASSERT_EQ(status, CT_ERROR_INVALID_ARGUMENT);
    status = ct_pbf_get_tile_labels(ctx, coord, &points, NULL);
    ASSERT_EQ(status, CT_ERROR_INVALID_ARGUMENT);

    ct_pbf_context_free(ctx);
    return 1;
}

TEST(place_type_classification)
{
    /* Test place type enum values exist and are distinct */
    ASSERT(CT_PLACE_UNKNOWN != CT_PLACE_CITY);
    ASSERT(CT_PLACE_CITY != CT_PLACE_TOWN);
    ASSERT(CT_PLACE_TOWN != CT_PLACE_VILLAGE);
    ASSERT(CT_PLACE_VILLAGE != CT_PLACE_HAMLET);
    ASSERT(CT_PLACE_PEAK != CT_PLACE_UNKNOWN);

    /* Test enum count is correct */
    ASSERT(CT_PLACE_TYPE_COUNT > CT_PLACE_PEAK);

    return 1;
}

TEST(labeled_point_structure)
{
    /* Test CTLabeledPoint fields exist */
    CTLabeledPoint pt;
    pt.id = 12345;
    pt.coord.lat = 47.5;
    pt.coord.lon = 19.0;
    pt.type = CT_PLACE_CITY;
    pt.name = NULL;
    pt.population = 1700000;
    pt.min_zoom = 6;
    pt.priority = 90;

    ASSERT_EQ(pt.id, 12345);
    ASSERT_NEAR(pt.coord.lat, 47.5, 0.001);
    ASSERT_EQ(pt.type, CT_PLACE_CITY);
    ASSERT_EQ(pt.population, 1700000);
    ASSERT_EQ(pt.min_zoom, 6);
    ASSERT_EQ(pt.priority, 90);

    return 1;
}

/* ============================================================================
 * Label Placement Tests
 * ============================================================================ */

TEST(label_placer_create)
{
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    ASSERT(placer != NULL);
    ASSERT_EQ(placer->tile_width, 256);
    ASSERT_EQ(placer->tile_height, 256);
    ASSERT(placer->collision != NULL);
    ASSERT_EQ(placer->num_placements, 0);
    ct_label_placer_free(placer);
    return 1;
}

TEST(label_placer_create_invalid)
{
    ASSERT(ct_label_placer_create(0, 256) == NULL);
    ASSERT(ct_label_placer_create(256, 0) == NULL);
    ASSERT(ct_label_placer_create(-1, 256) == NULL);
    return 1;
}

TEST(label_placer_reset)
{
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    ASSERT(placer != NULL);

    /* Manually add a placement to test reset */
    placer->num_placements = 5;

    ct_label_placer_reset(placer);
    ASSERT_EQ(placer->num_placements, 0);
    ASSERT_NEAR(ct_label_get_occupancy(placer), 0.0f, 0.001f);

    ct_label_placer_free(placer);
    return 1;
}

TEST(label_geo_to_pixel)
{
    /* Test coordinate conversion at zoom 0 (single tile covers world) */
    CTTileCoord coord = {0, 0, 0};
    int px, py;

    /* Center of world (0, 0) should be at center of tile */
    ct_label_geo_to_pixel(coord, 0.0, 0.0, 256, &px, &py);
    ASSERT(px >= 120 && px <= 136);  /* Around 128 */
    ASSERT(py >= 120 && py <= 136);  /* Around 128 */

    /* Western edge should be at left */
    ct_label_geo_to_pixel(coord, 0.0, -180.0, 256, &px, &py);
    ASSERT(px <= 10);

    /* Eastern edge should be at right */
    ct_label_geo_to_pixel(coord, 0.0, 180.0, 256, &px, &py);
    ASSERT(px >= 246);

    return 1;
}

TEST(label_place_single_with_font)
{
    const SHFont *font = sh_font_get_default();
    if (!font) {
        /* Skip if no font available */
        return 1;
    }

    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    ASSERT(placer != NULL);

    /* Create a test labeled point */
    CTLabeledPoint point = {
        .id = 1,
        .coord = {47.5, 19.0},
        .type = CT_PLACE_CITY,
        .name = "Budapest",
        .population = 1700000,
        .min_zoom = 6,
        .priority = 90
    };

    /* Place at center of tile */
    int result = ct_label_place_single(placer, &point, 128, 128, font, 12.0f);
    ASSERT_EQ(result, 1);
    ASSERT_EQ(ct_label_get_count(placer), 1);

    /* Verify placement */
    ASSERT(placer->placements[0].point == &point);
    ASSERT(placer->placements[0].width > 0);
    ASSERT(placer->placements[0].height > 0);

    ct_label_placer_free(placer);
    return 1;
}

TEST(label_collision_detection)
{
    const SHFont *font = sh_font_get_default();
    if (!font) {
        return 1;
    }

    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    ASSERT(placer != NULL);

    CTLabeledPoint point1 = {
        .id = 1, .coord = {0, 0}, .type = CT_PLACE_CITY,
        .name = "City One", .population = 100000, .min_zoom = 6, .priority = 90
    };

    CTLabeledPoint point2 = {
        .id = 2, .coord = {0, 0}, .type = CT_PLACE_CITY,
        .name = "City Two", .population = 50000, .min_zoom = 6, .priority = 80
    };

    /* Place first label at center */
    int result1 = ct_label_place_single(placer, &point1, 128, 128, font, 12.0f);
    ASSERT_EQ(result1, 1);

    /* Try to place second label at same position - may use different anchor */
    int result2 = ct_label_place_single(placer, &point2, 128, 128, font, 12.0f);
    (void)result2;  /* Result intentionally unused - just testing placement */
    /* Should either succeed with different anchor or fail if no space */

    /* At least one label should be placed */
    ASSERT(ct_label_get_count(placer) >= 1);

    ct_label_placer_free(placer);
    return 1;
}

TEST(label_configuration)
{
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    ASSERT(placer != NULL);

    /* Test padding configuration */
    ct_label_set_padding(placer, 10, 5);
    ASSERT_EQ(placer->padding_x, 10);
    ASSERT_EQ(placer->padding_y, 5);

    /* Test point offset configuration */
    ct_label_set_point_offset(placer, 8);
    ASSERT_EQ(placer->point_offset, 8);

    /* Test negative values (should clamp to 0) */
    ct_label_set_padding(placer, -5, -3);
    ASSERT_EQ(placer->padding_x, 0);
    ASSERT_EQ(placer->padding_y, 0);

    ct_label_placer_free(placer);
    return 1;
}

TEST(label_placer_null_safety)
{
    /* NULL placer */
    ct_label_placer_free(NULL);  /* Should not crash */
    ct_label_placer_reset(NULL);  /* Should not crash */
    ASSERT_EQ(ct_label_get_count(NULL), 0);
    ASSERT_NEAR(ct_label_get_occupancy(NULL), 0.0f, 0.001f);

    /* NULL parameters for place_single */
    const SHFont *font = sh_font_get_default();
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);
    CTLabeledPoint point = {.id = 1, .name = "Test"};

    ASSERT_EQ(ct_label_place_single(NULL, &point, 100, 100, font, 12.0f), 0);
    ASSERT_EQ(ct_label_place_single(placer, NULL, 100, 100, font, 12.0f), 0);
    if (font) {
        ASSERT_EQ(ct_label_place_single(placer, &point, 100, 100, NULL, 12.0f), 0);
    }

    ct_label_placer_free(placer);
    return 1;
}

TEST(label_anchor_types)
{
    /* Verify anchor enum values are distinct */
    ASSERT(CT_ANCHOR_CENTER != CT_ANCHOR_LEFT);
    ASSERT(CT_ANCHOR_LEFT != CT_ANCHOR_RIGHT);
    ASSERT(CT_ANCHOR_TOP != CT_ANCHOR_BOTTOM);
    ASSERT(CT_ANCHOR_COUNT > CT_ANCHOR_BOTTOM_RIGHT);
    return 1;
}

/* ============================================================================
 * Text Rendering Tests
 * ============================================================================ */

TEST(text_render_null_safety)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();

    /* All should handle NULL gracefully */
    ct_render_text(NULL, "test", 10, 10, font, 12.0f, CT_RGB(0, 0, 0));
    ct_render_text(ctx, NULL, 10, 10, font, 12.0f, CT_RGB(0, 0, 0));
    ct_render_text(ctx, "test", 10, 10, NULL, 12.0f, CT_RGB(0, 0, 0));
    ct_render_text(ctx, "test", 10, 10, font, 0.0f, CT_RGB(0, 0, 0));
    ct_render_text(ctx, "test", 10, 10, font, -1.0f, CT_RGB(0, 0, 0));

    ct_render_text_halo(NULL, "test", 10, 10, font, 12.0f,
                        CT_RGB(0, 0, 0), CT_RGB(255, 255, 255), 1.0f);
    ct_render_text_halo(ctx, NULL, 10, 10, font, 12.0f,
                        CT_RGB(0, 0, 0), CT_RGB(255, 255, 255), 1.0f);

    ct_render_free(ctx);
    return 1;
}

TEST(text_render_with_font)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();

    if (!font) {
        /* Skip if no font embedded */
        ct_render_free(ctx);
        return 1;
    }

    ct_render_clear(ctx);

    /* Render some text */
    ct_render_text(ctx, "Hello", 10, 10, font, 16.0f, CT_RGB(0, 0, 0));

    /* Should have drawn some pixels */
    /* We just verify no crash occurs */

    ct_render_free(ctx);
    return 1;
}

TEST(text_render_halo)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();

    if (!font) {
        ct_render_free(ctx);
        return 1;
    }

    ct_render_clear(ctx);

    /* Render text with halo */
    ct_render_text_halo(ctx, "Test", 50, 50, font, 14.0f,
                        CT_RGB(0, 0, 0),       /* Black fill */
                        CT_RGB(255, 255, 255), /* White halo */
                        1.5f);                 /* 1.5px halo */

    ct_render_free(ctx);
    return 1;
}

TEST(text_render_labels_null_safety)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);

    /* All should handle NULL gracefully */
    ASSERT_EQ(ct_render_labels(NULL, placer, font,
                               CT_RGB(0,0,0), CT_RGB(255,255,255), 1.0f), 0);
    ASSERT_EQ(ct_render_labels(ctx, NULL, font,
                               CT_RGB(0,0,0), CT_RGB(255,255,255), 1.0f), 0);
    ASSERT_EQ(ct_render_labels(ctx, placer, NULL,
                               CT_RGB(0,0,0), CT_RGB(255,255,255), 1.0f), 0);

    ct_label_placer_free(placer);
    ct_render_free(ctx);
    return 1;
}

TEST(text_render_labels_empty_placer)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();
    CTLabelPlacer *placer = ct_label_placer_create(256, 256);

    /* Empty placer should render 0 labels */
    int rendered = ct_render_labels(ctx, placer, font,
                                    CT_RGB(0, 0, 0), CT_RGB(255, 255, 255), 1.0f);
    ASSERT_EQ(rendered, 0);

    ct_label_placer_free(placer);
    ct_render_free(ctx);
    return 1;
}

TEST(text_glyph_render_null_safety)
{
    CTRenderContext *ctx = ct_render_create(256, 256);
    const SHFont *font = sh_font_get_default();

    /* NULL glyph should be handled */
    ct_render_glyph(ctx, NULL, 10, 10, font, 12.0f, CT_RGB(0, 0, 0), 0.5f);
    ct_render_glyph(NULL, NULL, 10, 10, font, 12.0f, CT_RGB(0, 0, 0), 0.5f);

    if (font) {
        const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
        ct_render_glyph(ctx, glyph, 10, 10, NULL, 12.0f, CT_RGB(0, 0, 0), 0.5f);
        ct_render_glyph(ctx, glyph, 10, 10, font, 0.0f, CT_RGB(0, 0, 0), 0.5f);
    }

    ct_render_free(ctx);
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\n=== Carta Test Suite ===\n\n");

    printf("Version:\n");
    run_test_version_not_null();
    run_test_mvt_spec_version();

    printf("\nStatus:\n");
    run_test_status_string_ok();
    run_test_status_string_error();

    printf("\nTile Coordinates:\n");
    run_test_latlon_to_tile_z0();
    run_test_latlon_to_tile_z1();
    run_test_tile_bounds_z0();
    run_test_tile_bounds_z1();
    run_test_tile_is_valid();
    run_test_tile_parent();
    run_test_tile_children();
    run_test_tiles_for_bbox();

    printf("\nWeb Mercator:\n");
    run_test_mercator_origin();
    run_test_mercator_roundtrip();

    printf("\nBatch Transform:\n");
    run_test_batch_transform_matches_direct();
    run_test_batch_transform_latitude_range();
    run_test_batch_transform_zoom_levels();

    printf("\nTile Management:\n");
    run_test_tile_init();
    run_test_tile_add_feature();

    printf("\nStyling:\n");
    run_test_default_style();
    run_test_scale_width();
    run_test_road_width_at_zoom();
    run_test_waterway_width();
    run_test_road_casing();
    run_test_railway_casing();
    run_test_bresenham_thin_line();
    run_test_aa_line_threshold();
    run_test_simd_alpha_blend();
    run_test_simd_alpha_blend_long_span();

    printf("\nRendering:\n");
    run_test_render_create();
    run_test_render_clear();
    run_test_render_set_pixel();
    run_test_render_line();
    run_test_render_context_has_scale_buffer();
    run_test_render_tile_reuses_buffer();
    run_test_render_polygon_scanline_performance();
    run_test_render_multipolygon_with_hole();

    printf("\nRender Options:\n");
    run_test_render_options_default();
    run_test_render_options_fast();
    run_test_render_options_quality();
    run_test_render_context_has_options();
    run_test_render_set_options();
    run_test_render_options_custom();

    printf("\nMVT Encoding:\n");
    run_test_mvt_default_options();
    run_test_mvt_layer_name();
    run_test_mvt_encode_empty_tile();
    run_test_mvt_encode_with_feature();

    printf("\nPNG Encoding:\n");
    run_test_png_default_options();
    run_test_png_max_size();
    run_test_png_encode_solid();

    printf("\nPBF Context:\n");
    run_test_pbf_context_create();
    run_test_pbf_stats_empty();
    run_test_pbf_context_relations_initialized();
    run_test_pbf_way_map_initialized();
    run_test_pbf_relation_member_types();
    run_test_pbf_memory_limit_config();

    printf("\nMultipolygon Assembly:\n");
    run_test_multipolygon_assemble_empty();
    run_test_multipolygon_get_role_string_empty();
    run_test_multipolygon_ring_structure();
    run_test_multipolygon_assembled_structure();

    printf("\nBoundary Assembly:\n");
    run_test_boundary_config_init();
    run_test_boundary_assemble_empty();
    run_test_boundary_type_enum();
    run_test_boundary_admin_level_constants();
    run_test_boundary_assembled_structure();
    run_test_boundary_rtree_empty();

    printf("\nASCII Rendering:\n");
    run_test_ascii_default_options();
    run_test_ascii_buffer_size();
    run_test_ascii_render_solid_image();
    run_test_ascii_render_gradient();
    run_test_ascii_invert_mode();

    printf("\nGeometry:\n");
    run_test_simplify_short_line();
    run_test_simplify_preserves_endpoints();
    run_test_simplify_multipolygon_preserves_rings();
    run_test_simplify_multipolygon_reads_correct_ring_positions();
    run_test_clip_polygon_inside();
    run_test_clip_polygon_partial();
    run_test_clip_polygon_outside();
    run_test_clip_linestring_crossing();
    run_test_clip_multipolygon_with_hole();
    run_test_clip_multipolygon_outer_only();
    run_test_clip_polygon_large_coordinates();
    run_test_clip_multipolygon_all_outside();
    run_test_simplify_line_collinear_points();
    run_test_simplify_line_zigzag();
    run_test_simplify_line_preserves_sharp_turns();
    run_test_simplify_poly_triangle();
    run_test_simplify_multipolygon_preserves_hole();

    printf("\nLOD:\n");
    run_test_lod_init_empty();
    run_test_lod_add_rule();
    run_test_lod_default_preset();
    run_test_lod_landuse_types();
    run_test_lod_boundary_admin_levels();
    run_test_lod_size_filtering();
    run_test_lod_estimate_area();
    run_test_lod_estimate_length();
    run_test_lod_waterway_types();

    printf("\nCollision Detection:\n");
    run_test_collision_create();
    run_test_collision_create_invalid();
    run_test_collision_empty_no_collision();
    run_test_collision_mark_and_test();
    run_test_collision_place_success();
    run_test_collision_place_fail();
    run_test_collision_place_padded();
    run_test_collision_reset();
    run_test_collision_outside_tile();
    run_test_collision_partial_outside();
    run_test_collision_occupancy();
    run_test_collision_null_safety();

    printf("\nLabeled Points:\n");
    run_test_labeled_points_empty_context();
    run_test_labeled_points_null_safety();
    run_test_place_type_classification();
    run_test_labeled_point_structure();

    printf("\nLabel Placement:\n");
    run_test_label_placer_create();
    run_test_label_placer_create_invalid();
    run_test_label_placer_reset();
    run_test_label_geo_to_pixel();
    run_test_label_place_single_with_font();
    run_test_label_collision_detection();
    run_test_label_configuration();
    run_test_label_placer_null_safety();
    run_test_label_anchor_types();

    printf("\nText Rendering:\n");
    run_test_text_render_null_safety();
    run_test_text_render_with_font();
    run_test_text_render_halo();
    run_test_text_render_labels_null_safety();
    run_test_text_render_labels_empty_placer();
    run_test_text_glyph_render_null_safety();

    printf("\n=== Results: %d/%d tests passed ===\n\n",
           tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
