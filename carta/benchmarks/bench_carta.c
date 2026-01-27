/*
 * bench_carta.c - Carta benchmarks
 */

#include "carta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static void bench_tile_math(void)
{
    printf("Tile coordinate math:\n");

    int iterations = 1000000;
    double start = get_time_ms();

    int x, y;
    for (int i = 0; i < iterations; i++) {
        double lat = (i % 170) - 85.0;
        double lon = (i % 360) - 180.0;
        ct_latlon_to_tile(lat, lon, 14, &x, &y);
    }

    double elapsed = get_time_ms() - start;
    printf("  latlon_to_tile: %.2f M ops/sec\n",
           iterations / elapsed / 1000.0);

    start = get_time_ms();
    for (int i = 0; i < iterations; i++) {
        CTTileCoord tile = {14, i % 16384, i % 16384};
        CTBBox bbox = ct_tile_bounds(tile);
        (void)bbox;
    }

    elapsed = get_time_ms() - start;
    printf("  tile_bounds:    %.2f M ops/sec\n",
           iterations / elapsed / 1000.0);
}

static void bench_rendering(void)
{
    printf("\nRendering:\n");

    CTRenderContext *ctx = ct_render_create(256, 256);
    if (!ctx) {
        printf("  Failed to create render context\n");
        return;
    }

    /* Benchmark line drawing */
    int lines = 10000;
    double start = get_time_ms();

    for (int i = 0; i < lines; i++) {
        ct_render_line(ctx, i % 256, 0, 255 - (i % 256), 255,
                       CT_RGB(255, 0, 0), 2.0f);
    }

    double elapsed = get_time_ms() - start;
    printf("  Line drawing (w=2): %.0f lines/sec\n", lines / elapsed * 1000);

    /* Benchmark polygon filling */
    ct_render_clear(ctx);
    CTTilePoint triangle[] = {{50, 50}, {200, 50}, {125, 200}};

    int polys = 10000;
    start = get_time_ms();

    for (int i = 0; i < polys; i++) {
        ct_render_polygon(ctx, triangle, 3, CT_RGB(0, 255, 0));
    }

    elapsed = get_time_ms() - start;
    printf("  Polygon fill:       %.0f polys/sec\n", polys / elapsed * 1000);

    ct_render_free(ctx);
}

static void bench_png_encoding(void)
{
    printf("\nPNG encoding:\n");

    int sizes[] = {64, 128, 256, 512};

    for (int s = 0; s < 4; s++) {
        int size = sizes[s];
        CTRenderContext *ctx = ct_render_create(size, size);
        ct_render_clear(ctx);

        /* Draw some content */
        for (int i = 0; i < 50; i++) {
            ct_render_line(ctx, i * size / 50, 0, size - i * size / 50, size,
                           CT_RGB(100, 150, 200), 1.0f);
        }

        size_t capacity = ct_png_max_size(size, size);
        uint8_t *buffer = malloc(capacity);

        int iterations = 100;
        double start = get_time_ms();

        for (int i = 0; i < iterations; i++) {
            size_t png_size = ct_encode_png(ct_render_pixels(ctx),
                                            size, size, NULL,
                                            buffer, capacity);
            (void)png_size;
        }

        double elapsed = get_time_ms() - start;
        printf("  %dx%d: %.1f encodes/sec (%.1f ms/encode)\n",
               size, size, iterations / elapsed * 1000, elapsed / iterations);

        free(buffer);
        ct_render_free(ctx);
    }
}

static void bench_mvt_encoding(void)
{
    printf("\nMVT encoding:\n");

    /* Create a tile with varying numbers of features */
    int feature_counts[] = {10, 100, 1000};

    for (int f = 0; f < 3; f++) {
        int num_features = feature_counts[f];

        CTTile tile;
        ct_tile_init(&tile, (CTTileCoord){14, 1000, 1000});

        /* Add features */
        for (int i = 0; i < num_features; i++) {
            CTTilePoint *points = malloc(5 * sizeof(CTTilePoint));
            points[0] = (CTTilePoint){(i * 100) % 4096, (i * 50) % 4096};
            points[1] = (CTTilePoint){(i * 100 + 500) % 4096, (i * 50 + 200) % 4096};
            points[2] = (CTTilePoint){(i * 100 + 1000) % 4096, (i * 50) % 4096};
            points[3] = (CTTilePoint){(i * 100 + 500) % 4096, (i * 50 + 500) % 4096};
            points[4] = points[0];

            CTFeature feature = {
                .type = CT_GEOM_LINESTRING,
                .points = points,
                .num_points = 5,
                .layer = CT_LAYER_ROADS,
                .feature_type = i % 8
            };
            ct_tile_add_feature(&tile, &feature);
        }

        uint8_t *buffer = malloc(1024 * 1024);

        int iterations = 100;
        double start = get_time_ms();
        size_t last_size = 0;

        for (int i = 0; i < iterations; i++) {
            last_size = ct_encode_mvt(&tile, NULL, buffer, 1024 * 1024);
        }

        double elapsed = get_time_ms() - start;
        printf("  %d features: %.1f encodes/sec (%.1f ms, %zu bytes)\n",
               num_features, iterations / elapsed * 1000,
               elapsed / iterations, last_size);

        free(buffer);
        ct_tile_free(&tile);
    }
}

static void bench_pbf_loading(const char *filename)
{
    printf("\nPBF loading (%s):\n", filename);

    double start = get_time_ms();
    CTPBFContext *ctx = ct_load_pbf(filename);
    double elapsed = get_time_ms() - start;

    if (!ctx) {
        printf("  Failed to load PBF file\n");
        return;
    }

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(ctx, &nodes, &ways, &features, &bbox);

    printf("  Load time: %.1f sec\n", elapsed / 1000);
    printf("  Nodes: %zu, Ways: %zu, Features: %zu\n", nodes, ways, features);
    printf("  Bounds: [%.4f, %.4f] to [%.4f, %.4f]\n",
           bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat);

    /* Benchmark tile generation */
    printf("\n  Tile generation (z14):\n");

    /* Find center of bbox for test tile */
    double center_lat = (bbox.min_lat + bbox.max_lat) / 2;
    double center_lon = (bbox.min_lon + bbox.max_lon) / 2;
    int tx, ty;
    ct_latlon_to_tile(center_lat, center_lon, 14, &tx, &ty);

    CTTileCoord test_tile = {14, tx, ty};

    /* MVT generation */
    uint8_t *mvt_buf = malloc(1024 * 1024);
    int mvt_iters = 50;
    start = get_time_ms();

    for (int i = 0; i < mvt_iters; i++) {
        size_t size = ct_generate_mvt(ctx, test_tile, NULL, mvt_buf, 1024 * 1024);
        (void)size;
    }

    elapsed = get_time_ms() - start;
    printf("    MVT: %.1f ms/tile\n", elapsed / mvt_iters);

    /* PNG generation */
    size_t png_capacity = ct_png_max_size(256, 256);
    uint8_t *png_buf = malloc(png_capacity);
    int png_iters = 20;
    start = get_time_ms();

    for (int i = 0; i < png_iters; i++) {
        size_t size = ct_generate_png(ctx, test_tile, NULL, NULL,
                                      png_buf, png_capacity);
        (void)size;
    }

    elapsed = get_time_ms() - start;
    printf("    PNG: %.1f ms/tile\n", elapsed / png_iters);

    free(mvt_buf);
    free(png_buf);
    ct_free_pbf_context(ctx);
}

int main(int argc, char *argv[])
{
    printf("\n=== Carta Benchmarks ===\n\n");

    bench_tile_math();
    bench_rendering();
    bench_png_encoding();
    bench_mvt_encoding();

    if (argc > 1) {
        bench_pbf_loading(argv[1]);
    } else {
        printf("\nTo benchmark PBF loading, run: ./bench_carta <file.osm.pbf>\n");
    }

    printf("\n");
    return 0;
}
