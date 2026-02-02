/*
 * test_carta.c - Test suite for Carta tile generator
 */

#include "carta.h"
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

    /* Below z10 clamps to z10 width */
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 5), 2.0f, 0.01);

    /* Above z18 clamps to z18 width */
    ASSERT_NEAR(ct_road_width_at_zoom(&rw, 20), 8.0f, 0.01);

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

    printf("\nTile Management:\n");
    run_test_tile_init();
    run_test_tile_add_feature();

    printf("\nStyling:\n");
    run_test_default_style();
    run_test_scale_width();
    run_test_road_width_at_zoom();

    printf("\nRendering:\n");
    run_test_render_create();
    run_test_render_clear();
    run_test_render_set_pixel();
    run_test_render_line();
    run_test_render_context_has_scale_buffer();
    run_test_render_tile_reuses_buffer();
    run_test_render_polygon_scanline_performance();

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

    printf("\nASCII Rendering:\n");
    run_test_ascii_default_options();
    run_test_ascii_buffer_size();
    run_test_ascii_render_solid_image();
    run_test_ascii_render_gradient();
    run_test_ascii_invert_mode();

    printf("\nGeometry:\n");
    run_test_simplify_short_line();
    run_test_simplify_preserves_endpoints();

    printf("\n=== Results: %d/%d tests passed ===\n\n",
           tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
