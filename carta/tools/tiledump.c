/*
 * tiledump - pre-render Carta PNG tiles for a bbox / zoom range to disk.
 *
 * Renders a static tile pyramid ({outdir}/{z}/{x}/{y}.png) that any slippy-map
 * client (Leaflet, OpenLayers, ...) can serve as a local, offline basemap --
 * no tile server required. Used by surge/scripts/surge_map.py to produce a
 * self-contained route map.
 *
 *   tiledump <graph.osm.pbf|map.idx> <outdir> <zmin> <zmax>
 *            <minlat> <minlon> <maxlat> <maxlon>
 *
 * Tiles are 256x256 (ct_generate_png_lod default). Loading a large .osm.pbf
 * takes tens of seconds; build a .idx once (carta-tile-server --save-index) for
 * instant loads.
 */
#include "carta.h"
#include "ct_png.h"
#include "ct_tile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

static int lon2x(double lon, int z) {
    return (int)floor((lon + 180.0) / 360.0 * (double)(1 << z));
}
static int lat2y(double lat, int z) {
    double r = lat * M_PI / 180.0;
    return (int)floor((1.0 - asinh(tan(r)) / M_PI) / 2.0 * (double)(1 << z));
}

int main(int argc, char **argv) {
    if (argc < 9) {
        fprintf(stderr, "usage: %s <graph.osm.pbf|map.idx> <outdir> <zmin> <zmax> "
                        "<minlat> <minlon> <maxlat> <maxlon>\n", argv[0]);
        return 2;
    }
    const char *src = argv[1], *outdir = argv[2];
    int zmin = atoi(argv[3]), zmax = atoi(argv[4]);
    double minlat = atof(argv[5]), minlon = atof(argv[6]);
    double maxlat = atof(argv[7]), maxlon = atof(argv[8]);
    if (zmin < 0 || zmax < zmin || zmax > 20) {
        fprintf(stderr, "tiledump: bad zoom range %d..%d\n", zmin, zmax);
        return 2;
    }

    fprintf(stderr, "tiledump: loading %s ...\n", src);
    CTPBFContext *ctx = ct_load_pbf(src);
    if (!ctx) { fprintf(stderr, "tiledump: load failed\n"); return 1; }
    fprintf(stderr, "tiledump: loaded; rendering z%d..%d\n", zmin, zmax);

    size_t cap = 8u << 20;
    uint8_t *buf = malloc(cap);
    if (!buf) { ct_free_pbf_context(ctx); return 1; }

    char path[1024];
    int total = 0, empty = 0;
    mkdir(outdir, 0755);
    for (int z = zmin; z <= zmax; z++) {
        int x0 = lon2x(minlon, z), x1 = lon2x(maxlon, z);
        int y0 = lat2y(maxlat, z), y1 = lat2y(minlat, z); /* y grows southward */
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        snprintf(path, sizeof(path), "%s/%d", outdir, z); mkdir(path, 0755);
        int zc = 0;
        for (int x = x0; x <= x1; x++) {
            snprintf(path, sizeof(path), "%s/%d/%d", outdir, z, x); mkdir(path, 0755);
            for (int y = y0; y <= y1; y++) {
                CTTileCoord c = { z, x, y };
                size_t n = ct_generate_png_lod(ctx, c, NULL, NULL, NULL, buf, cap);
                if (!n) { empty++; continue; }
                snprintf(path, sizeof(path), "%s/%d/%d/%d.png", outdir, z, x, y);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(buf, 1, n, f); fclose(f); total++; zc++; }
            }
        }
        fprintf(stderr, "tiledump:   z%d: %d tiles (x %d..%d, y %d..%d)\n", z, zc, x0, x1, y0, y1);
    }
    fprintf(stderr, "tiledump: done, %d tiles written (%d empty/failed)\n", total, empty);
    free(buf);
    ct_free_pbf_context(ctx);
    return 0;
}
