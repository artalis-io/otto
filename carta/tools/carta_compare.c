/*
 * carta_compare.c - Tile comparison tool for debugging render quality
 *
 * Compares carta PNG/MVT output against OSM reference tiles.
 * Designed for use in a Claude agent feedback loop.
 *
 * Usage:
 *   ./carta-compare info monaco.osm.pbf              # Show map bounds and valid tiles
 *   ./carta-compare 14/8527/5979 monaco.osm.pbf      # Compare single tile
 *   ./carta-compare batch monaco.osm.pbf             # Compare sample tiles at all zooms
 *   ./carta-compare batch monaco.osm.pbf --zoom 12   # Compare tiles at zoom 12
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <math.h>
#include <curl/curl.h>

#include "carta.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_lod.h"

/* ============================================================================
 * Safe String Macros
 * ============================================================================ */

#define SAFE_STRCPY(dst, src) do { \
    strncpy((dst), (src), sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0'; \
} while(0)

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef enum {
    MODE_SINGLE,    /* Compare single tile */
    MODE_INFO,      /* Show map bounds and valid tiles */
    MODE_BATCH      /* Compare multiple tiles */
} CompareMode;

typedef struct {
    CompareMode mode;
    int z, x, y;              /* Tile coordinates (for MODE_SINGLE) */
    char pbf_path[512];       /* Path to PBF/index file */
    char output_dir[512];     /* Output directory (default: /tmp/carta_compare) */
    int mvt_mode;             /* 1 = MVT, 0 = PNG */
    int tile_size;            /* PNG tile size (256 or 512) */
    int zoom_level;           /* For batch mode: specific zoom (-1 = all) */
    int max_tiles;            /* Max tiles per zoom in batch mode */
    int verbose;              /* Verbose output */
    int skip_osm;             /* Skip fetching OSM tiles */
} CompareConfig;

/* ============================================================================
 * Curl Response Buffer
 * ============================================================================ */

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} CurlBuffer;

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    /* Check for integer overflow */
    if (size > 0 && nmemb > SIZE_MAX / size) {
        return 0;
    }
    size_t realsize = size * nmemb;
    CurlBuffer *buf = (CurlBuffer *)userp;

    /* Grow buffer if needed */
    if (buf->size + realsize > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap < buf->size + realsize) {
            new_cap = buf->size + realsize + 4096;
        }
        uint8_t *new_data = realloc(buf->data, new_cap);
        if (!new_data) {
            fprintf(stderr, "Error: Out of memory\n");
            return 0;
        }
        buf->data = new_data;
        buf->capacity = new_cap;
    }

    memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    return realsize;
}

/* ============================================================================
 * File I/O
 * ============================================================================ */

static int write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s for writing\n", path);
        return -1;
    }
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return (written == size) ? 0 : -1;
}

static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return mkdir(path, 0755);
}

/* ============================================================================
 * Tile Bounds Validation
 * ============================================================================ */

typedef struct {
    CTBBox bbox;              /* Geographic bounds */
    int min_x[23], max_x[23]; /* Valid X range per zoom */
    int min_y[23], max_y[23]; /* Valid Y range per zoom */
} TileBounds;

static void compute_tile_bounds(const CTPBFContext *pbf, TileBounds *bounds)
{
    /* Get geographic bounds from PBF */
    size_t nodes, ways, features;
    ct_pbf_stats(pbf, &nodes, &ways, &features, &bounds->bbox);

    /* Compute valid tile ranges for each zoom level */
    for (int z = 0; z <= 22; z++) {
        ct_latlon_to_tile(bounds->bbox.max_lat, bounds->bbox.min_lon, z,
                          &bounds->min_x[z], &bounds->min_y[z]);
        ct_latlon_to_tile(bounds->bbox.min_lat, bounds->bbox.max_lon, z,
                          &bounds->max_x[z], &bounds->max_y[z]);
    }
}

static int tile_in_bounds(const TileBounds *bounds, int z, int x, int y)
{
    if (z < 0 || z > 22) return 0;
    return x >= bounds->min_x[z] && x <= bounds->max_x[z] &&
           y >= bounds->min_y[z] && y <= bounds->max_y[z];
}

/* ============================================================================
 * OSM Tile Fetching
 * ============================================================================ */

static int fetch_osm_tile(int z, int x, int y, CurlBuffer *buf)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://tile.openstreetmap.org/%d/%d/%d.png", z, x, y);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Error: Failed to initialize curl\n");
        return -1;
    }

    buf->data = malloc(64 * 1024);
    buf->size = 0;
    buf->capacity = 64 * 1024;
    if (!buf->data) {
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "carta-compare/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf->data);
        buf->data = NULL;
        return -1;
    }

    if (http_code != 200) {
        free(buf->data);
        buf->data = NULL;
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Tile Statistics
 * ============================================================================ */

typedef struct {
    size_t num_ways;
    size_t num_multipolygons;
    size_t num_water;
    size_t num_roads;
    size_t num_buildings;
    size_t num_landuse;
    double render_time_ms;
    size_t output_size;
} TileStats;

/* Count features by layer from the PBF context for a tile */
static void count_tile_features(const CTPBFContext *pbf, CTTileCoord coord,
                                TileStats *stats)
{
    CTFeature *features = NULL;
    size_t count = 0;

    CTLODConfig lod;
    ct_lod_init(&lod);
    ct_lod_default(&lod);

    if (ct_pbf_get_tile_features_lod(pbf, coord, &lod, &features, &count) == CT_OK) {
        for (size_t i = 0; i < count; i++) {
            switch (features[i].layer) {
                case CT_LAYER_WATER:
                    stats->num_water++;
                    break;
                case CT_LAYER_ROADS:
                    stats->num_roads++;
                    break;
                case CT_LAYER_BUILDINGS:
                    stats->num_buildings++;
                    break;
                case CT_LAYER_LANDUSE:
                    stats->num_landuse++;
                    break;
                default:
                    break;
            }
            free(features[i].points);
            if (features[i].ring_ends) free(features[i].ring_ends);
        }
        free(features);
    }

    ct_lod_free(&lod);
}

/* ============================================================================
 * Tile Generation
 * ============================================================================ */

static size_t generate_carta_png(const CTPBFContext *pbf, CTTileCoord coord,
                                 int tile_size, uint8_t *buffer, size_t capacity,
                                 TileStats *stats)
{
    struct timeval start, end;
    gettimeofday(&start, NULL);

    CTLODConfig lod;
    ct_lod_init(&lod);
    ct_lod_default(&lod);

    CTPNGOptions opts;
    ct_png_default_options(&opts);
    opts.tile_size = tile_size;

    size_t size = ct_generate_png_lod(pbf, coord, NULL, &lod, &opts,
                                       buffer, capacity);

    gettimeofday(&end, NULL);

    stats->render_time_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;
    stats->num_ways = pbf->num_ways;
    stats->num_multipolygons = pbf->num_multipolygons;
    stats->output_size = size;

    ct_lod_free(&lod);
    return size;
}

static size_t generate_carta_mvt(const CTPBFContext *pbf, CTTileCoord coord,
                                 uint8_t *buffer, size_t capacity,
                                 TileStats *stats)
{
    struct timeval start, end;
    gettimeofday(&start, NULL);

    CTLODConfig lod;
    ct_lod_init(&lod);
    ct_lod_default(&lod);

    CTMVTOptions opts;
    ct_mvt_default_options(&opts);

    size_t size = ct_generate_mvt(pbf, coord, &opts, &lod, buffer, capacity);

    gettimeofday(&end, NULL);

    stats->render_time_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;
    stats->num_ways = pbf->num_ways;
    stats->num_multipolygons = pbf->num_multipolygons;
    stats->output_size = size;

    ct_lod_free(&lod);
    return size;
}

/* ============================================================================
 * Mode: Info - Show map bounds and valid tiles
 * ============================================================================ */

static int run_info_mode(const CTPBFContext *pbf, const CompareConfig *cfg)
{
    (void)cfg;

    TileBounds bounds;
    compute_tile_bounds(pbf, &bounds);

    printf("=== Map Info ===\n\n");
    printf("Geographic Bounds:\n");
    printf("  Latitude:  %.6f to %.6f\n", bounds.bbox.min_lat, bounds.bbox.max_lat);
    printf("  Longitude: %.6f to %.6f\n", bounds.bbox.min_lon, bounds.bbox.max_lon);
    printf("\n");

    printf("Statistics:\n");
    printf("  Ways: %zu\n", pbf->num_ways);
    printf("  Multipolygons: %zu\n", pbf->num_multipolygons);
    printf("  Labeled points: %zu\n", ct_pbf_get_label_count(pbf));
    printf("\n");

    printf("Valid Tile Ranges:\n");
    printf("  Zoom |    X Range    |    Y Range    | Tile Count\n");
    printf("  -----|---------------|---------------|------------\n");

    for (int z = 0; z <= 18; z++) {
        int x_count = bounds.max_x[z] - bounds.min_x[z] + 1;
        int y_count = bounds.max_y[z] - bounds.min_y[z] + 1;
        long long total = (long long)x_count * y_count;

        if (total <= 10000) {
            printf("    %2d | %5d - %-5d | %5d - %-5d | %lld\n",
                   z, bounds.min_x[z], bounds.max_x[z],
                   bounds.min_y[z], bounds.max_y[z], total);
        } else {
            printf("    %2d | %5d - %-5d | %5d - %-5d | %lld (large)\n",
                   z, bounds.min_x[z], bounds.max_x[z],
                   bounds.min_y[z], bounds.max_y[z], total);
        }
    }
    printf("\n");

    /* Suggest sample tiles at various zooms */
    printf("Sample Tiles (center of map):\n");
    double center_lat = (bounds.bbox.min_lat + bounds.bbox.max_lat) / 2;
    double center_lon = (bounds.bbox.min_lon + bounds.bbox.max_lon) / 2;

    for (int z = 10; z <= 16; z += 2) {
        int x, y;
        ct_latlon_to_tile(center_lat, center_lon, z, &x, &y);
        printf("  ./carta-compare %d/%d/%d %s\n", z, x, y, cfg->pbf_path);
    }
    printf("\n");

    return 0;
}

/* ============================================================================
 * Mode: Single Tile Comparison
 * ============================================================================ */

static int run_single_mode(const CTPBFContext *pbf, const CompareConfig *cfg,
                           uint8_t *buffer, size_t buffer_capacity)
{
    TileBounds bounds;
    compute_tile_bounds(pbf, &bounds);

    /* Validate tile is in bounds */
    if (!tile_in_bounds(&bounds, cfg->z, cfg->x, cfg->y)) {
        fprintf(stderr, "Error: Tile %d/%d/%d is outside map bounds\n",
                cfg->z, cfg->x, cfg->y);
        fprintf(stderr, "Valid range at zoom %d: x=%d-%d, y=%d-%d\n",
                cfg->z, bounds.min_x[cfg->z], bounds.max_x[cfg->z],
                bounds.min_y[cfg->z], bounds.max_y[cfg->z]);
        return 1;
    }

    CTTileCoord coord = {cfg->z, cfg->x, cfg->y};
    TileStats stats = {0};

    /* Count features by layer */
    count_tile_features(pbf, coord, &stats);

    printf("=== Tile Comparison: %d/%d/%d ===\n\n", cfg->z, cfg->x, cfg->y);

    /* Generate carta tile */
    printf("Generating carta tile...\n");
    size_t carta_size;
    if (cfg->mvt_mode) {
        carta_size = generate_carta_mvt(pbf, coord, buffer, buffer_capacity, &stats);
    } else {
        carta_size = generate_carta_png(pbf, coord, cfg->tile_size, buffer,
                                        buffer_capacity, &stats);
    }

    const char *ext = cfg->mvt_mode ? "mvt" : "png";
    char carta_path[1024];
    char osm_path[1024];

    if (carta_size == 0) {
        if (cfg->mvt_mode) {
            printf("Note: No features found in tile area (empty MVT)\n\n");
        } else {
            fprintf(stderr, "Error: Failed to generate carta tile\n");
            return 1;
        }
    } else {
        /* Save carta tile */
        snprintf(carta_path, sizeof(carta_path), "%s/carta_%d_%d_%d.%s",
                 cfg->output_dir, cfg->z, cfg->x, cfg->y, ext);
        if (write_file(carta_path, buffer, carta_size) != 0) {
            return 1;
        }
    }

    /* Fetch OSM reference tile (PNG only) */
    CurlBuffer osm_buf = {0};
    if (!cfg->mvt_mode && !cfg->skip_osm) {
        printf("Fetching OSM reference tile...\n");
        if (fetch_osm_tile(cfg->z, cfg->x, cfg->y, &osm_buf) != 0) {
            fprintf(stderr, "Warning: Failed to fetch OSM tile\n");
        } else {
            snprintf(osm_path, sizeof(osm_path), "%s/osm_%d_%d_%d.png",
                     cfg->output_dir, cfg->z, cfg->x, cfg->y);
            write_file(osm_path, osm_buf.data, osm_buf.size);
        }
    }

    /* Print structured results for Claude */
    printf("\n");
    printf("--- TILE REPORT ---\n");
    printf("Tile: %d/%d/%d\n", cfg->z, cfg->x, cfg->y);
    printf("Format: %s\n", ext);
    if (carta_size > 0) {
        printf("Carta Output: %s (%.1f KB)\n", carta_path, carta_size / 1024.0);
    } else {
        printf("Carta Output: (empty)\n");
    }
    if (!cfg->mvt_mode && osm_buf.data) {
        printf("OSM Reference: %s (%.1f KB)\n", osm_path, osm_buf.size / 1024.0);
    }
    printf("\n");
    printf("--- FEATURE COUNTS ---\n");
    printf("Water features: %zu\n", stats.num_water);
    printf("Road features: %zu\n", stats.num_roads);
    printf("Building features: %zu\n", stats.num_buildings);
    printf("Landuse features: %zu\n", stats.num_landuse);
    printf("\n");
    printf("--- PERFORMANCE ---\n");
    printf("Render time: %.2f ms\n", stats.render_time_ms);
    printf("Output size: %zu bytes\n", carta_size);
    printf("--- END REPORT ---\n");

    if (osm_buf.data) free(osm_buf.data);
    return 0;
}

/* ============================================================================
 * Mode: Batch Comparison
 * ============================================================================ */

static int run_batch_mode(const CTPBFContext *pbf, const CompareConfig *cfg,
                          uint8_t *buffer, size_t buffer_capacity)
{
    TileBounds bounds;
    compute_tile_bounds(pbf, &bounds);

    printf("=== Batch Tile Comparison ===\n\n");

    int min_zoom = cfg->zoom_level >= 0 ? cfg->zoom_level : 10;
    int max_zoom = cfg->zoom_level >= 0 ? cfg->zoom_level : 16;
    int max_tiles = cfg->max_tiles > 0 ? cfg->max_tiles : 3;

    printf("Zoom range: %d-%d\n", min_zoom, max_zoom);
    printf("Max tiles per zoom: %d\n", max_tiles);
    printf("Format: %s\n\n", cfg->mvt_mode ? "MVT" : "PNG");

    int total_tiles = 0;
    int total_errors = 0;
    double total_time = 0.0;

    for (int z = min_zoom; z <= max_zoom; z++) {
        int x_count = bounds.max_x[z] - bounds.min_x[z] + 1;
        int y_count = bounds.max_y[z] - bounds.min_y[z] + 1;

        /* Sample tiles: corners and center */
        int sample_x[5], sample_y[5];
        int num_samples = 0;

        /* Center */
        sample_x[num_samples] = (bounds.min_x[z] + bounds.max_x[z]) / 2;
        sample_y[num_samples] = (bounds.min_y[z] + bounds.max_y[z]) / 2;
        num_samples++;

        /* Corners (if map is large enough) */
        if (x_count > 1 && y_count > 1) {
            /* Top-left */
            sample_x[num_samples] = bounds.min_x[z];
            sample_y[num_samples] = bounds.min_y[z];
            num_samples++;

            /* Bottom-right */
            sample_x[num_samples] = bounds.max_x[z];
            sample_y[num_samples] = bounds.max_y[z];
            num_samples++;
        }

        /* Quarter points for more coverage */
        if (x_count >= 4 && y_count >= 4 && num_samples < max_tiles) {
            sample_x[num_samples] = bounds.min_x[z] + x_count / 4;
            sample_y[num_samples] = bounds.min_y[z] + y_count / 4;
            num_samples++;

            sample_x[num_samples] = bounds.max_x[z] - x_count / 4;
            sample_y[num_samples] = bounds.max_y[z] - y_count / 4;
            num_samples++;
        }

        if (num_samples > max_tiles) num_samples = max_tiles;

        printf("--- Zoom %d (%d samples) ---\n", z, num_samples);

        for (int i = 0; i < num_samples; i++) {
            CTTileCoord coord = {z, sample_x[i], sample_y[i]};
            TileStats stats = {0};

            /* Count features */
            count_tile_features(pbf, coord, &stats);

            /* Generate tile */
            size_t size;
            if (cfg->mvt_mode) {
                size = generate_carta_mvt(pbf, coord, buffer, buffer_capacity, &stats);
            } else {
                size = generate_carta_png(pbf, coord, cfg->tile_size, buffer,
                                          buffer_capacity, &stats);
            }

            /* Save tile */
            const char *ext = cfg->mvt_mode ? "mvt" : "png";
            char path[1024];
            snprintf(path, sizeof(path), "%s/carta_%d_%d_%d.%s",
                     cfg->output_dir, z, sample_x[i], sample_y[i], ext);

            int success = 1;
            if (size > 0) {
                if (write_file(path, buffer, size) != 0) {
                    success = 0;
                    total_errors++;
                }
            }

            /* Report */
            printf("  %d/%d/%d: ", z, sample_x[i], sample_y[i]);
            if (size == 0) {
                printf("(empty)");
            } else {
                printf("%.1f KB, %.1f ms", size / 1024.0, stats.render_time_ms);
            }
            printf(" [water:%zu roads:%zu bldg:%zu land:%zu]",
                   stats.num_water, stats.num_roads, stats.num_buildings, stats.num_landuse);
            if (!success) printf(" ERROR");
            printf("\n");

            total_tiles++;
            total_time += stats.render_time_ms;
        }
        printf("\n");
    }

    printf("=== Summary ===\n");
    printf("Total tiles: %d\n", total_tiles);
    printf("Total errors: %d\n", total_errors);
    printf("Total render time: %.1f ms\n", total_time);
    printf("Average render time: %.2f ms\n", total_tiles > 0 ? total_time / total_tiles : 0);
    printf("Output directory: %s/\n", cfg->output_dir);

    return total_errors > 0 ? 1 : 0;
}

/* ============================================================================
 * Argument Parsing
 * ============================================================================ */

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <command> <pbf-file> [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  info <pbf>              Show map bounds and valid tile ranges\n");
    fprintf(stderr, "  <z/x/y> <pbf>           Compare single tile against OSM reference\n");
    fprintf(stderr, "  batch <pbf>             Compare sample tiles at multiple zooms\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --mvt                   Generate MVT instead of PNG\n");
    fprintf(stderr, "  -o, --output DIR        Output directory (default: /tmp/carta_compare)\n");
    fprintf(stderr, "  -s, --size SIZE         PNG tile size: 256 or 512 (default: 512)\n");
    fprintf(stderr, "  -z, --zoom LEVEL        Batch mode: specific zoom level\n");
    fprintf(stderr, "  -n, --max-tiles N       Batch mode: max tiles per zoom (default: 3)\n");
    fprintf(stderr, "  --skip-osm              Don't fetch OSM reference tiles\n");
    fprintf(stderr, "  -v, --verbose           Verbose output\n");
    fprintf(stderr, "  -h, --help              Show this help\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s info monaco.osm.pbf\n", prog);
    fprintf(stderr, "  %s 14/8527/5979 monaco.osm.pbf\n", prog);
    fprintf(stderr, "  %s batch monaco.osm.pbf --zoom 14\n", prog);
    fprintf(stderr, "  %s batch monaco.osm.pbf -n 5 -o /tmp/tiles/\n", prog);
}

static int parse_tile_coords(const char *str, int *z, int *x, int *y)
{
    return sscanf(str, "%d/%d/%d", z, x, y) == 3 ? 0 : -1;
}

static int parse_args(int argc, char **argv, CompareConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->tile_size = 512;
    cfg->zoom_level = -1;
    cfg->max_tiles = 3;
    SAFE_STRCPY(cfg->output_dir, "/tmp/carta_compare");

    if (argc < 3) {
        print_usage(argv[0]);
        return -1;
    }

    /* Parse command/mode */
    if (strcmp(argv[1], "info") == 0) {
        cfg->mode = MODE_INFO;
        SAFE_STRCPY(cfg->pbf_path, argv[2]);
    } else if (strcmp(argv[1], "batch") == 0) {
        cfg->mode = MODE_BATCH;
        SAFE_STRCPY(cfg->pbf_path, argv[2]);
    } else if (parse_tile_coords(argv[1], &cfg->z, &cfg->x, &cfg->y) == 0) {
        cfg->mode = MODE_SINGLE;
        SAFE_STRCPY(cfg->pbf_path, argv[2]);
    } else {
        fprintf(stderr, "Error: Unknown command or invalid tile coordinates '%s'\n", argv[1]);
        fprintf(stderr, "Expected: 'info', 'batch', or 'z/x/y' (e.g., 14/9058/5729)\n");
        return -1;
    }

    /* Parse options */
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--mvt") == 0) {
            cfg->mvt_mode = 1;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i-1]);
                return -1;
            }
            SAFE_STRCPY(cfg->output_dir, argv[i]);
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i-1]);
                return -1;
            }
            cfg->tile_size = atoi(argv[i]);
            if (cfg->tile_size != 256 && cfg->tile_size != 512) {
                fprintf(stderr, "Error: Tile size must be 256 or 512\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--zoom") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i-1]);
                return -1;
            }
            cfg->zoom_level = atoi(argv[i]);
            if (cfg->zoom_level < 0 || cfg->zoom_level > 22) {
                fprintf(stderr, "Error: Zoom level must be 0-22\n");
                return -1;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--max-tiles") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i-1]);
                return -1;
            }
            cfg->max_tiles = atoi(argv[i]);
            if (cfg->max_tiles < 1 || cfg->max_tiles > 100) {
                fprintf(stderr, "Error: Max tiles must be 1-100\n");
                return -1;
            }
        } else if (strcmp(argv[i], "--skip-osm") == 0) {
            cfg->skip_osm = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv)
{
    CompareConfig cfg;
    if (parse_args(argc, argv, &cfg) != 0) {
        return 1;
    }

    /* Initialize curl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Load PBF */
    printf("Loading %s...\n", cfg.pbf_path);
    CTPBFContext *pbf = ct_load_pbf(cfg.pbf_path);
    if (!pbf) {
        fprintf(stderr, "Error: Failed to load PBF file\n");
        curl_global_cleanup();
        return 1;
    }
    printf("Loaded: %zu ways, %zu multipolygons\n\n",
           pbf->num_ways, pbf->num_multipolygons);

    int result = 0;

    if (cfg.mode == MODE_INFO) {
        result = run_info_mode(pbf, &cfg);
    } else {
        /* Ensure output directory exists */
        if (ensure_dir(cfg.output_dir) != 0) {
            fprintf(stderr, "Error: Cannot create output directory %s\n",
                    cfg.output_dir);
            ct_free_pbf_context(pbf);
            curl_global_cleanup();
            return 1;
        }

        /* Allocate tile buffer */
        size_t buffer_capacity = 4 * 1024 * 1024; /* 4 MB */
        uint8_t *buffer = malloc(buffer_capacity);
        if (!buffer) {
            fprintf(stderr, "Error: Out of memory\n");
            ct_free_pbf_context(pbf);
            curl_global_cleanup();
            return 1;
        }

        if (cfg.mode == MODE_SINGLE) {
            result = run_single_mode(pbf, &cfg, buffer, buffer_capacity);
        } else if (cfg.mode == MODE_BATCH) {
            result = run_batch_mode(pbf, &cfg, buffer, buffer_capacity);
        }

        free(buffer);
    }

    ct_free_pbf_context(pbf);
    curl_global_cleanup();

    return result;
}
