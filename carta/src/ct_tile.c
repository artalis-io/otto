/*
 * ct_tile.c - Tile coordinate math and Web Mercator projection
 */

#include "ct_tile.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

/* ============================================================================
 * Web Mercator Projection
 * ============================================================================ */

void ct_latlon_to_mercator(double lat, double lon, double *x, double *y)
{
    /* Clamp latitude to valid Mercator range */
    if (lat > 85.051128779806) lat = 85.051128779806;
    if (lat < -85.051128779806) lat = -85.051128779806;

    double lat_rad = lat * CT_PI / 180.0;

    *x = CT_EARTH_RADIUS_M * lon * CT_PI / 180.0;
    *y = CT_EARTH_RADIUS_M * log(tan(CT_PI / 4.0 + lat_rad / 2.0));
}

void ct_mercator_to_latlon(double x, double y, double *lat, double *lon)
{
    *lon = x / CT_EARTH_RADIUS_M * 180.0 / CT_PI;
    *lat = (2.0 * atan(exp(y / CT_EARTH_RADIUS_M)) - CT_PI / 2.0) * 180.0 / CT_PI;
}

/* ============================================================================
 * Tile Coordinate Functions
 * ============================================================================ */

void ct_latlon_to_tile(double lat, double lon, int zoom,
                       int *tile_x, int *tile_y)
{
    /* Clamp latitude */
    if (lat > 85.051128779806) lat = 85.051128779806;
    if (lat < -85.051128779806) lat = -85.051128779806;

    double n = (double)(1 << zoom);
    double lat_rad = lat * CT_PI / 180.0;

    *tile_x = (int)((lon + 180.0) / 360.0 * n);
    *tile_y = (int)((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / CT_PI) / 2.0 * n);

    /* Clamp to valid range */
    int max_tile = (1 << zoom) - 1;
    if (*tile_x < 0) *tile_x = 0;
    if (*tile_x > max_tile) *tile_x = max_tile;
    if (*tile_y < 0) *tile_y = 0;
    if (*tile_y > max_tile) *tile_y = max_tile;
}

void ct_latlon_to_tile_pixel(double lat, double lon, CTTileCoord tile,
                             int extent, int *px, int *py)
{
    /* Clamp latitude */
    if (lat > 85.051128779806) lat = 85.051128779806;
    if (lat < -85.051128779806) lat = -85.051128779806;

    double n = (double)(1 << tile.z);
    double lat_rad = lat * CT_PI / 180.0;

    /* Global pixel position */
    double global_x = (lon + 180.0) / 360.0 * n;
    double global_y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / CT_PI) / 2.0 * n;

    /* Relative to tile */
    *px = (int)((global_x - tile.x) * extent);
    *py = (int)((global_y - tile.y) * extent);
}

/* ============================================================================
 * Mercator Lookup Table
 * ============================================================================ */

/*
 * Lookup table for fast latitude -> Mercator Y conversion.
 * Covers the valid Web Mercator range [-85.051, 85.051] degrees.
 * Uses 64K entries for ~0.0026 degree resolution (~290m at equator).
 */
#define MERCATOR_LUT_SIZE 65536
#define MERCATOR_LAT_MIN -85.051128779806
#define MERCATOR_LAT_MAX  85.051128779806
#define MERCATOR_LAT_RANGE (MERCATOR_LAT_MAX - MERCATOR_LAT_MIN)

static double mercator_lut[MERCATOR_LUT_SIZE];
static pthread_once_t mercator_lut_once = PTHREAD_ONCE_INIT;

/*
 * Initialize the Mercator lookup table (thread-safe via pthread_once).
 */
static void mercator_lut_init_impl(void)
{
    for (int i = 0; i < MERCATOR_LUT_SIZE; i++) {
        double lat = MERCATOR_LAT_MIN + (i + 0.5) * MERCATOR_LAT_RANGE / MERCATOR_LUT_SIZE;
        double lat_rad = lat * CT_PI / 180.0;
        mercator_lut[i] = log(tan(lat_rad) + 1.0 / cos(lat_rad));
    }
}

static void mercator_lut_init(void)
{
    pthread_once(&mercator_lut_once, mercator_lut_init_impl);
}

/*
 * Fast Mercator Y lookup with linear interpolation.
 * Input: latitude in degrees [-85.051, 85.051]
 * Output: Mercator Y value
 */
static inline double fast_mercator_y(double lat)
{
    /* Clamp to valid range */
    if (lat <= MERCATOR_LAT_MIN) return mercator_lut[0];
    if (lat >= MERCATOR_LAT_MAX) return mercator_lut[MERCATOR_LUT_SIZE - 1];

    /* Map latitude to table index */
    double idx_f = (lat - MERCATOR_LAT_MIN) * (MERCATOR_LUT_SIZE - 1) / MERCATOR_LAT_RANGE;
    int idx = (int)idx_f;
    double frac = idx_f - idx;

    /* Bounds check for safety */
    if (idx < 0) idx = 0;
    if (idx >= MERCATOR_LUT_SIZE - 1) return mercator_lut[MERCATOR_LUT_SIZE - 1];

    /* Linear interpolation */
    return mercator_lut[idx] + frac * (mercator_lut[idx + 1] - mercator_lut[idx]);
}

/* ============================================================================
 * Fast Batch Coordinate Transformation
 * ============================================================================ */

/*
 * Pre-computed transformation coefficients for a tile.
 * Allows fast batch conversion of lat/lon to tile pixels.
 */
typedef struct {
    /* Longitude transformation: px = lon * lon_scale + lon_offset */
    double lon_scale;
    double lon_offset;

    /* Latitude transformation coefficients */
    double lat_to_py_scale;  /* Scale factor */
    double lat_to_py_offset; /* Offset */

    int extent;
} CTTileTransform;

/*
 * Helper: compute Mercator y from latitude (radians) - used for init only
 */
static inline double lat_to_mercator_y(double lat_rad)
{
    return log(tan(lat_rad) + 1.0 / cos(lat_rad));
}

/*
 * Initialize tile transform for fast batch conversion.
 */
static void ct_tile_transform_init(CTTileTransform *tf, CTTileCoord tile, int extent)
{
    /* Ensure lookup table is ready */
    mercator_lut_init();

    double n = (double)(1 << tile.z);

    /* Longitude is linear: px = (lon + 180) / 360 * n * extent - tile.x * extent */
    tf->lon_scale = n * extent / 360.0;
    tf->lon_offset = 180.0 * tf->lon_scale - tile.x * extent;

    /* For latitude, we use the lookup table with precomputed scale:
     * py = (1 - merc_y / PI) / 2 * n * extent - tile.y * extent
     * py = -merc_y * (n * extent / (2 * PI)) + (n * extent / 2) - tile.y * extent
     */
    tf->lat_to_py_scale = -n * extent / (2.0 * CT_PI);
    tf->lat_to_py_offset = n * extent / 2.0 - tile.y * extent;

    tf->extent = extent;
}

/*
 * Fast batch conversion of fixed-point lat/lon to tile pixels.
 * Points are in nanodegrees (int32 * 1e-7 = degrees).
 * Uses lookup table for Mercator projection - no trig calls.
 */
void ct_batch_transform_points(CTTileCoord tile, int extent,
                               CTTilePoint *points, int num_points)
{
    CTTileTransform tf;
    ct_tile_transform_init(&tf, tile, extent);

    for (int i = 0; i < num_points; i++) {
        /* Points stored as nanodegrees in x (lon) and y (lat) */
        double lon = points[i].x * 1e-7;
        double lat = points[i].y * 1e-7;

        /* Longitude: simple linear transform */
        int px = (int)(lon * tf.lon_scale + tf.lon_offset);

        /* Latitude: Mercator projection via lookup table (no trig!) */
        double merc_y = fast_mercator_y(lat);
        int py = (int)(merc_y * tf.lat_to_py_scale + tf.lat_to_py_offset);

        points[i].x = px;
        points[i].y = py;
    }
}

CTBBox ct_tile_bounds(CTTileCoord tile)
{
    double n = (double)(1 << tile.z);

    double min_lon = tile.x / n * 360.0 - 180.0;
    double max_lon = (tile.x + 1) / n * 360.0 - 180.0;

    double min_lat_rad = atan(sinh(CT_PI * (1 - 2 * (tile.y + 1) / n)));
    double max_lat_rad = atan(sinh(CT_PI * (1 - 2 * tile.y / n)));

    CTBBox bbox;
    bbox.min_lon = min_lon;
    bbox.max_lon = max_lon;
    bbox.min_lat = min_lat_rad * 180.0 / CT_PI;
    bbox.max_lat = max_lat_rad * 180.0 / CT_PI;

    return bbox;
}

void ct_tile_to_latlon(CTTileCoord tile, double *lat, double *lon)
{
    double n = (double)(1 << tile.z);

    *lon = (tile.x + 0.5) / n * 360.0 - 180.0;

    double lat_rad = atan(sinh(CT_PI * (1 - 2 * (tile.y + 0.5) / n)));
    *lat = lat_rad * 180.0 / CT_PI;
}

/* ============================================================================
 * Tile Enumeration
 * ============================================================================ */

int ct_tiles_for_bbox(CTBBox bbox, int zoom, CTTileCoord **tiles)
{
    int min_x, min_y, max_x, max_y;

    ct_latlon_to_tile(bbox.max_lat, bbox.min_lon, zoom, &min_x, &min_y);
    ct_latlon_to_tile(bbox.min_lat, bbox.max_lon, zoom, &max_x, &max_y);

    int num_x = max_x - min_x + 1;
    int num_y = max_y - min_y + 1;
    int total = num_x * num_y;

    *tiles = malloc(total * sizeof(CTTileCoord));
    if (!*tiles) return 0;

    int idx = 0;
    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            (*tiles)[idx].z = zoom;
            (*tiles)[idx].x = x;
            (*tiles)[idx].y = y;
            idx++;
        }
    }

    return total;
}

CTTileCoord ct_tile_parent(CTTileCoord tile)
{
    CTTileCoord parent;
    parent.z = tile.z > 0 ? tile.z - 1 : 0;
    parent.x = tile.x / 2;
    parent.y = tile.y / 2;
    return parent;
}

void ct_tile_children(CTTileCoord tile, CTTileCoord children[4])
{
    int z = tile.z + 1;
    int x = tile.x * 2;
    int y = tile.y * 2;

    children[0] = (CTTileCoord){z, x, y};
    children[1] = (CTTileCoord){z, x + 1, y};
    children[2] = (CTTileCoord){z, x, y + 1};
    children[3] = (CTTileCoord){z, x + 1, y + 1};
}

int ct_tile_is_valid(CTTileCoord tile)
{
    if (tile.z < 0 || tile.z > CT_MAX_ZOOM) return 0;

    int max_coord = 1 << tile.z;
    if (tile.x < 0 || tile.x >= max_coord) return 0;
    if (tile.y < 0 || tile.y >= max_coord) return 0;

    return 1;
}

/* ============================================================================
 * Tile Management
 * ============================================================================ */

void ct_tile_init(CTTile *tile, CTTileCoord coord)
{
    memset(tile, 0, sizeof(CTTile));
    tile->coord = coord;
}

CTStatus ct_tile_add_feature(CTTile *tile, const CTFeature *feature)
{
    if (tile->num_features >= tile->features_capacity) {
        size_t new_cap = tile->features_capacity ? tile->features_capacity * 2 : 64;
        CTFeature *new_features = realloc(tile->features,
                                          new_cap * sizeof(CTFeature));
        if (!new_features) return CT_ERROR_OUT_OF_MEMORY;
        tile->features = new_features;
        tile->features_capacity = new_cap;
    }

    tile->features[tile->num_features++] = *feature;
    return CT_OK;
}

void ct_tile_clear(CTTile *tile)
{
    /* Free feature point arrays */
    for (size_t i = 0; i < tile->num_features; i++) {
        free(tile->features[i].points);
        free(tile->features[i].ring_ends);
        if (tile->features[i].prop_keys) {
            for (int j = 0; j < tile->features[i].num_props; j++) {
                free(tile->features[i].prop_keys[j]);
                free(tile->features[i].prop_values[j]);
            }
            free(tile->features[i].prop_keys);
            free(tile->features[i].prop_values);
        }
    }
    tile->num_features = 0;
    tile->point_pool_size = 0;
}

void ct_tile_free(CTTile *tile)
{
    ct_tile_clear(tile);
    free(tile->features);
    free(tile->point_pool);
    memset(tile, 0, sizeof(CTTile));
}

/* ============================================================================
 * Geometry Clipping (Sutherland-Hodgman)
 * ============================================================================ */

/* Cohen-Sutherland outcodes */
#define OUTCODE_INSIDE 0
#define OUTCODE_LEFT   1
#define OUTCODE_RIGHT  2
#define OUTCODE_BOTTOM 4
#define OUTCODE_TOP    8

static int compute_outcode(int x, int y, int min, int max)
{
    int code = OUTCODE_INSIDE;
    if (x < min) code |= OUTCODE_LEFT;
    else if (x > max) code |= OUTCODE_RIGHT;
    if (y < min) code |= OUTCODE_BOTTOM;
    else if (y > max) code |= OUTCODE_TOP;
    return code;
}

void ct_clip_linestring(const CTTilePoint *points, int num_points,
                        int extent, int buffer,
                        CTTilePoint **out, int *out_count,
                        int **segments, int *seg_count)
{
    int min = -buffer;
    int max = extent + buffer;

    /* Allocate output (worst case: same size) */
    *out = malloc(num_points * 2 * sizeof(CTTilePoint));
    *segments = malloc(num_points * sizeof(int));
    if (!*out || !*segments) {
        free(*out);
        free(*segments);
        *out = NULL;
        *segments = NULL;
        *out_count = 0;
        *seg_count = 0;
        return;
    }

    *out_count = 0;
    *seg_count = 0;
    int in_segment = 0;

    for (int i = 0; i < num_points - 1; i++) {
        int x0 = points[i].x, y0 = points[i].y;
        int x1 = points[i + 1].x, y1 = points[i + 1].y;

        int code0 = compute_outcode(x0, y0, min, max);
        int code1 = compute_outcode(x1, y1, min, max);

        int accept = 0;

        while (1) {
            if (!(code0 | code1)) {
                /* Both inside */
                accept = 1;
                break;
            } else if (code0 & code1) {
                /* Both outside same region */
                break;
            } else {
                /* Line needs clipping */
                int code_out = code0 ? code0 : code1;
                int x, y;

                if (code_out & OUTCODE_TOP) {
                    x = x0 + (x1 - x0) * (max - y0) / (y1 - y0);
                    y = max;
                } else if (code_out & OUTCODE_BOTTOM) {
                    x = x0 + (x1 - x0) * (min - y0) / (y1 - y0);
                    y = min;
                } else if (code_out & OUTCODE_RIGHT) {
                    y = y0 + (y1 - y0) * (max - x0) / (x1 - x0);
                    x = max;
                } else {
                    y = y0 + (y1 - y0) * (min - x0) / (x1 - x0);
                    x = min;
                }

                if (code_out == code0) {
                    x0 = x;
                    y0 = y;
                    code0 = compute_outcode(x0, y0, min, max);
                } else {
                    x1 = x;
                    y1 = y;
                    code1 = compute_outcode(x1, y1, min, max);
                }
            }
        }

        if (accept) {
            if (!in_segment) {
                (*out)[*out_count].x = x0;
                (*out)[*out_count].y = y0;
                (*out_count)++;
                in_segment = 1;
            }
            (*out)[*out_count].x = x1;
            (*out)[*out_count].y = y1;
            (*out_count)++;
        } else if (in_segment) {
            (*segments)[*seg_count] = *out_count;
            (*seg_count)++;
            in_segment = 0;
        }
    }

    if (in_segment && *out_count > 0) {
        (*segments)[*seg_count] = *out_count;
        (*seg_count)++;
    }
}

void ct_clip_polygon(const CTTilePoint *points, int num_points,
                     int extent, int buffer,
                     CTTilePoint **out, int *out_count)
{
    /* Sutherland-Hodgman polygon clipping */
    int min = -buffer;
    int max = extent + buffer;

    /* Allocate temporary buffers */
    CTTilePoint *input = malloc(num_points * 4 * sizeof(CTTilePoint));
    CTTilePoint *output = malloc(num_points * 4 * sizeof(CTTilePoint));
    if (!input || !output) {
        free(input);
        free(output);
        *out = NULL;
        *out_count = 0;
        return;
    }

    memcpy(input, points, num_points * sizeof(CTTilePoint));
    int input_count = num_points;

    /* Clip against each edge */
    int edges[4][4] = {
        {min, min, min, max},  /* Left */
        {min, max, max, max},  /* Top */
        {max, max, max, min},  /* Right */
        {max, min, min, min}   /* Bottom */
    };

    for (int e = 0; e < 4; e++) {
        if (input_count == 0) break;

        int output_count = 0;
        int x1 = edges[e][0], y1 = edges[e][1];
        int x2 = edges[e][2], y2 = edges[e][3];

        CTTilePoint prev = input[input_count - 1];

        for (int i = 0; i < input_count; i++) {
            CTTilePoint curr = input[i];

            /* Check if points are inside edge */
            int prev_inside = (x2 - x1) * (prev.y - y1) - (y2 - y1) * (prev.x - x1) >= 0;
            int curr_inside = (x2 - x1) * (curr.y - y1) - (y2 - y1) * (curr.x - x1) >= 0;

            if (curr_inside) {
                if (!prev_inside) {
                    /* Compute intersection */
                    double t = ((double)(x2 - x1) * (prev.y - y1) - (double)(y2 - y1) * (prev.x - x1)) /
                               ((double)(y2 - y1) * (curr.x - prev.x) - (double)(x2 - x1) * (curr.y - prev.y));
                    output[output_count].x = (int)(prev.x + t * (curr.x - prev.x));
                    output[output_count].y = (int)(prev.y + t * (curr.y - prev.y));
                    output_count++;
                }
                output[output_count++] = curr;
            } else if (prev_inside) {
                /* Compute intersection */
                double t = ((double)(x2 - x1) * (prev.y - y1) - (double)(y2 - y1) * (prev.x - x1)) /
                           ((double)(y2 - y1) * (curr.x - prev.x) - (double)(x2 - x1) * (curr.y - prev.y));
                output[output_count].x = (int)(prev.x + t * (curr.x - prev.x));
                output[output_count].y = (int)(prev.y + t * (curr.y - prev.y));
                output_count++;
            }

            prev = curr;
        }

        /* Swap buffers */
        CTTilePoint *tmp = input;
        input = output;
        output = tmp;
        input_count = output_count;
    }

    *out = malloc(input_count * sizeof(CTTilePoint));
    if (*out) {
        memcpy(*out, input, input_count * sizeof(CTTilePoint));
        *out_count = input_count;
    } else {
        *out_count = 0;
    }

    free(input);
    free(output);
}

/* ============================================================================
 * Geometry Simplification (Douglas-Peucker)
 * ============================================================================ */

static double point_line_distance(CTTilePoint p, CTTilePoint a, CTTilePoint b)
{
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double len_sq = dx * dx + dy * dy;

    if (len_sq == 0) {
        dx = p.x - a.x;
        dy = p.y - a.y;
        return sqrt(dx * dx + dy * dy);
    }

    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq;
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    double proj_x = a.x + t * dx;
    double proj_y = a.y + t * dy;

    dx = p.x - proj_x;
    dy = p.y - proj_y;
    return sqrt(dx * dx + dy * dy);
}

static void douglas_peucker(const CTTilePoint *points, int start, int end,
                            double tolerance, int *keep)
{
    if (end - start < 2) return;

    double max_dist = 0;
    int max_idx = start;

    for (int i = start + 1; i < end; i++) {
        double dist = point_line_distance(points[i], points[start], points[end]);
        if (dist > max_dist) {
            max_dist = dist;
            max_idx = i;
        }
    }

    if (max_dist > tolerance) {
        keep[max_idx] = 1;
        douglas_peucker(points, start, max_idx, tolerance, keep);
        douglas_peucker(points, max_idx, end, tolerance, keep);
    }
}

void ct_simplify_linestring(const CTTilePoint *points, int num_points,
                            double tolerance,
                            CTTilePoint **out, int *out_count)
{
    if (num_points < 3) {
        *out = malloc(num_points * sizeof(CTTilePoint));
        if (*out) {
            memcpy(*out, points, num_points * sizeof(CTTilePoint));
            *out_count = num_points;
        } else {
            *out_count = 0;
        }
        return;
    }

    int *keep = calloc(num_points, sizeof(int));
    if (!keep) {
        *out = NULL;
        *out_count = 0;
        return;
    }

    keep[0] = 1;
    keep[num_points - 1] = 1;
    douglas_peucker(points, 0, num_points - 1, tolerance, keep);

    /* Count kept points */
    int count = 0;
    for (int i = 0; i < num_points; i++) {
        if (keep[i]) count++;
    }

    *out = malloc(count * sizeof(CTTilePoint));
    if (!*out) {
        free(keep);
        *out_count = 0;
        return;
    }

    int idx = 0;
    for (int i = 0; i < num_points; i++) {
        if (keep[i]) {
            (*out)[idx++] = points[i];
        }
    }

    *out_count = count;
    free(keep);
}
