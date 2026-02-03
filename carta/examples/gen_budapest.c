/*
 * gen_budapest.c - Example: Generate map tiles of Budapest
 *
 * Usage:
 *   cd carta
 *   make lib
 *   gcc -O3 -Iinclude -Ivendor examples/gen_budapest.c -L. -lcarta -lm -o gen_budapest
 *   ./gen_budapest ../velo/hungary-latest.osm.pbf
 */

#include "carta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char *argv[])
{
    const char *pbf_file = argc > 1 ? argv[1] : "../velo/hungary-latest.osm.pbf";

    printf("Carta %s - Budapest Tile Generator\n\n", ct_version());
    printf("Loading %s...\n", pbf_file);

    double start = get_time();
    CTPBFContext *ctx = ct_load_pbf(pbf_file);
    double load_time = get_time() - start;

    if (!ctx) {
        printf("Failed to load PBF file: %s\n", pbf_file);
        return 1;
    }

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(ctx, &nodes, &ways, &features, &bbox);
    printf("Loaded in %.1f sec: %zu nodes, %zu ways, %zu features\n",
           load_time, nodes, ways, features);
    printf("Bounds: [%.4f, %.4f] to [%.4f, %.4f]\n\n",
           bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat);

    /* Budapest center: 47.4979, 19.0402 */
    double center_lat = 47.4979;
    double center_lon = 19.0402;
    int zoom = 14;

    int tx, ty;
    ct_latlon_to_tile(center_lat, center_lon, zoom, &tx, &ty);
    printf("Budapest center tile at z%d: %d/%d/%d\n", zoom, zoom, tx, ty);

    CTTileCoord tile = {zoom, tx, ty};
    CTBBox tile_bbox = ct_tile_bounds(tile);
    printf("Tile bounds: [%.4f, %.4f] to [%.4f, %.4f]\n\n",
           tile_bbox.min_lon, tile_bbox.min_lat,
           tile_bbox.max_lon, tile_bbox.max_lat);

    /* Generate PNG tile */
    printf("Generating PNG tiles (512x512)...\n");

    CTPNGOptions png_opts;
    ct_png_default_options(&png_opts);
    png_opts.tile_size = 512;

    size_t capacity = ct_png_max_size(512, 512);
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        printf("Out of memory!\n");
        ct_free_pbf_context(ctx);
        return 1;
    }

    /* Generate 3x3 grid around Budapest center */
    int generated = 0;
    start = get_time();

    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            CTTileCoord t = {zoom, tx + dx, ty + dy};
            char filename[64];
            snprintf(filename, sizeof(filename), "budapest_%d_%d_%d.png",
                     t.z, t.x, t.y);

            size_t size = ct_generate_png(ctx, t, NULL, &png_opts,
                                          buffer, capacity);
            if (size > 0) {
                FILE *f = fopen(filename, "wb");
                if (f) {
                    fwrite(buffer, 1, size, f);
                    fclose(f);
                    printf("  %s (%zu bytes)\n", filename, size);
                    generated++;
                }
            }
        }
    }

    double render_time = get_time() - start;
    printf("\nGenerated %d tiles in %.2f sec (%.0f ms/tile)\n",
           generated, render_time, render_time / generated * 1000);

    /* Also generate MVT for comparison */
    printf("\nGenerating MVT tile...\n");
    start = get_time();
    size_t mvt_size = ct_generate_mvt(ctx, tile, NULL, NULL, buffer, capacity);
    double mvt_time = get_time() - start;

    if (mvt_size > 0) {
        char filename[64];
        snprintf(filename, sizeof(filename), "budapest_%d_%d_%d.mvt",
                 tile.z, tile.x, tile.y);
        FILE *f = fopen(filename, "wb");
        if (f) {
            fwrite(buffer, 1, mvt_size, f);
            fclose(f);
            printf("  %s (%zu bytes) in %.1f ms\n", filename, mvt_size, mvt_time * 1000);
        }
    }

    free(buffer);
    ct_free_pbf_context(ctx);

    printf("\nDone! Open the PNG files to see the results.\n");
    return 0;
}
