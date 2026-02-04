/*
 * ct_mvt.c - Mapbox Vector Tile encoding
 *
 * Encodes tile data to MVT format (Protocol Buffers).
 * See: https://github.com/mapbox/vector-tile-spec
 */

#include "ct_mvt.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_simplify.h"
#include "ct_lod.h"
#include "sh_protobuf.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* MVT field numbers */
#define MVT_TILE_LAYERS         3

#define MVT_LAYER_VERSION       15
#define MVT_LAYER_NAME          1
#define MVT_LAYER_FEATURES      2
#define MVT_LAYER_KEYS          3
#define MVT_LAYER_VALUES        4
#define MVT_LAYER_EXTENT        5

#define MVT_FEATURE_ID          1
#define MVT_FEATURE_TAGS        2
#define MVT_FEATURE_TYPE        3
#define MVT_FEATURE_GEOMETRY    4

#define MVT_VALUE_STRING        1
#define MVT_VALUE_FLOAT         2
#define MVT_VALUE_DOUBLE        3
#define MVT_VALUE_INT           4
#define MVT_VALUE_UINT          5
#define MVT_VALUE_SINT          6
#define MVT_VALUE_BOOL          7

/* Geometry commands */
#define MVT_CMD_MOVETO          1
#define MVT_CMD_LINETO          2
#define MVT_CMD_CLOSEPATH       7

/* ============================================================================
 * Default Options
 * ============================================================================ */

void ct_mvt_default_options(CTMVTOptions *opts)
{
    opts->extent = CT_MVT_EXTENT;
    opts->buffer = 64;
    opts->simplify = 1;
    opts->tolerance = 4.0;
    opts->compress = 0;
}

/* ============================================================================
 * Layer Names
 * ============================================================================ */

const char *ct_mvt_layer_name(CTLayer layer)
{
    switch (layer) {
        case CT_LAYER_ROADS:      return "roads";
        case CT_LAYER_WATER:      return "water";
        case CT_LAYER_BUILDINGS:  return "buildings";
        case CT_LAYER_LANDUSE:    return "landuse";
        case CT_LAYER_RAILWAYS:   return "railways";
        case CT_LAYER_BOUNDARIES: return "boundaries";
        case CT_LAYER_LABELS:     return "labels";
        default:                  return "other";
    }
}

/* ============================================================================
 * Encoder Context
 * ============================================================================ */

void ct_mvt_encoder_init(CTMVTEncoder *enc, uint8_t *buffer, size_t capacity)
{
    enc->buffer = buffer;
    enc->capacity = capacity;
    enc->offset = 0;
    enc->error = 0;
}

static void enc_write_varint(CTMVTEncoder *enc, uint64_t value)
{
    if (enc->error) return;
    if (enc->offset >= enc->capacity) {
        enc->error = 1;
        return;
    }
    int n = sh_pb_write_varint(enc->buffer + enc->offset,
                               enc->capacity - enc->offset, value);
    if (n <= 0) {
        enc->error = 1;
        return;
    }
    enc->offset += n;
}

static void enc_write_svarint(CTMVTEncoder *enc, int64_t value)
{
    /* Zigzag encode: cast to unsigned before left shift to avoid UB */
    uint64_t uval = ((uint64_t)value << 1) ^ (uint64_t)(value >> 63);
    enc_write_varint(enc, uval);
}

static void enc_write_tag(CTMVTEncoder *enc, uint32_t field, uint32_t wire)
{
    enc_write_varint(enc, ((uint64_t)field << 3) | wire);
}

static void enc_write_bytes(CTMVTEncoder *enc, const uint8_t *data, size_t len)
{
    if (enc->error) return;
    if (enc->offset + len > enc->capacity) {
        enc->error = 1;
        return;
    }
    memcpy(enc->buffer + enc->offset, data, len);
    enc->offset += len;
}

/* Reserve space in the buffer without writing. Returns 1 on success, 0 on error. */
static int enc_reserve(CTMVTEncoder *enc, size_t len)
{
    if (enc->error) return 0;
    if (enc->offset + len > enc->capacity) {
        enc->error = 1;
        return 0;
    }
    enc->offset += len;
    return 1;
}

/* Maximum layer name length for MVT encoding */
#define MVT_MAX_STRING_LEN 256

static void enc_write_string(CTMVTEncoder *enc, const char *str)
{
    size_t len = strnlen(str, MVT_MAX_STRING_LEN);
    enc_write_varint(enc, len);
    enc_write_bytes(enc, (const uint8_t *)str, len);
}

/* ============================================================================
 * Geometry Encoding
 * ============================================================================ */

static void encode_geometry(CTMVTEncoder *enc, const CTFeature *feature)
{
    /* Defensive check: skip features with no points */
    if (!feature->points || feature->num_points == 0) {
        return;
    }

    /* Encode geometry as packed uint32 array */

    /* Skip length prefix for now */
    enc_write_tag(enc, MVT_FEATURE_GEOMETRY, 2);  /* length-delimited */
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;  /* Reserve space for length (up to 5 bytes) */

    size_t cmd_start = enc->offset;

    int32_t cx = 0, cy = 0;  /* Cursor position */

    if (feature->type == CT_GEOM_POINT) {
        /* MoveTo command */
        enc_write_varint(enc, (MVT_CMD_MOVETO << 3) | 1);

        int32_t dx = feature->points[0].x - cx;
        int32_t dy = feature->points[0].y - cy;
        enc_write_svarint(enc, dx);
        enc_write_svarint(enc, dy);
    } else if (feature->type == CT_GEOM_LINESTRING) {
        /* MoveTo first point */
        enc_write_varint(enc, (MVT_CMD_MOVETO << 3) | 1);
        int32_t dx = feature->points[0].x - cx;
        int32_t dy = feature->points[0].y - cy;
        enc_write_svarint(enc, dx);
        enc_write_svarint(enc, dy);
        cx = feature->points[0].x;
        cy = feature->points[0].y;

        /* LineTo remaining points */
        if (feature->num_points > 1) {
            enc_write_varint(enc, (MVT_CMD_LINETO << 3) | (feature->num_points - 1));

            for (int i = 1; i < feature->num_points; i++) {
                dx = feature->points[i].x - cx;
                dy = feature->points[i].y - cy;
                enc_write_svarint(enc, dx);
                enc_write_svarint(enc, dy);
                cx = feature->points[i].x;
                cy = feature->points[i].y;
            }
        }
    } else if (feature->type == CT_GEOM_POLYGON) {
        /* MoveTo first point */
        enc_write_varint(enc, (MVT_CMD_MOVETO << 3) | 1);
        int32_t dx = feature->points[0].x - cx;
        int32_t dy = feature->points[0].y - cy;
        enc_write_svarint(enc, dx);
        enc_write_svarint(enc, dy);
        cx = feature->points[0].x;
        cy = feature->points[0].y;

        /* LineTo remaining points (excluding last if it's the same as first) */
        int last_idx = feature->num_points - 1;
        if (feature->points[last_idx].x == feature->points[0].x &&
            feature->points[last_idx].y == feature->points[0].y) {
            last_idx--;
        }

        if (last_idx > 0) {
            enc_write_varint(enc, (MVT_CMD_LINETO << 3) | last_idx);

            for (int i = 1; i <= last_idx; i++) {
                dx = feature->points[i].x - cx;
                dy = feature->points[i].y - cy;
                enc_write_svarint(enc, dx);
                enc_write_svarint(enc, dy);
                cx = feature->points[i].x;
                cy = feature->points[i].y;
            }
        }

        /* ClosePath */
        enc_write_varint(enc, (MVT_CMD_CLOSEPATH << 3) | 1);
    }

    /* Write actual length */
    size_t cmd_len = enc->offset - cmd_start;
    size_t saved_offset = enc->offset;

    enc->offset = len_pos;
    enc_write_varint(enc, cmd_len);

    /* Move commands if length varint was shorter than 5 bytes */
    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, cmd_len);
        enc->offset = len_pos + len_size + cmd_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* ============================================================================
 * Feature Encoding
 * ============================================================================ */

static void encode_feature(CTMVTEncoder *enc, const CTFeature *feature, uint64_t id)
{
    /* Start feature message */
    enc_write_tag(enc, MVT_LAYER_FEATURES, 2);

    /* Skip length for now */
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;

    size_t content_start = enc->offset;

    /* Feature ID */
    enc_write_tag(enc, MVT_FEATURE_ID, 0);
    enc_write_varint(enc, id);

    /* Feature type */
    enc_write_tag(enc, MVT_FEATURE_TYPE, 0);
    enc_write_varint(enc, (uint64_t)feature->type);

    /* Geometry */
    encode_geometry(enc, feature);

    /* Write actual length */
    size_t content_len = enc->offset - content_start;
    size_t saved_offset = enc->offset;

    enc->offset = len_pos;
    enc_write_varint(enc, content_len);

    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, content_len);
        enc->offset = len_pos + len_size + content_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* ============================================================================
 * Layer Encoding
 * ============================================================================ */

static void encode_layer(CTMVTEncoder *enc, const char *name, int extent,
                         const CTFeature *features, size_t num_features)
{
    /* Start layer message */
    enc_write_tag(enc, MVT_TILE_LAYERS, 2);

    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;

    size_t content_start = enc->offset;

    /* Version (required, first) */
    enc_write_tag(enc, MVT_LAYER_VERSION, 0);
    enc_write_varint(enc, 2);

    /* Name */
    enc_write_tag(enc, MVT_LAYER_NAME, 2);
    enc_write_string(enc, name);

    /* Extent */
    enc_write_tag(enc, MVT_LAYER_EXTENT, 0);
    enc_write_varint(enc, extent);

    /* Features */
    for (size_t i = 0; i < num_features; i++) {
        encode_feature(enc, &features[i], i + 1);
    }

    /* Write actual length */
    size_t content_len = enc->offset - content_start;
    size_t saved_offset = enc->offset;

    enc->offset = len_pos;
    enc_write_varint(enc, content_len);

    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, content_len);
        enc->offset = len_pos + len_size + content_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* ============================================================================
 * Public Encoding Functions
 * ============================================================================ */

size_t ct_encode_mvt(const CTTile *tile, const CTMVTOptions *opts,
                     uint8_t *buffer, size_t capacity)
{
    if (!tile || !buffer) return 0;

    CTMVTOptions default_opts;
    if (!opts) {
        ct_mvt_default_options(&default_opts);
        opts = &default_opts;
    }

    CTMVTEncoder enc;
    ct_mvt_encoder_init(&enc, buffer, capacity);

    if (tile->num_features == 0) {
        return enc.offset;
    }

    /*
     * Single allocation for all layer features instead of per-layer mallocs.
     * This reduces malloc overhead from O(CT_LAYER_COUNT) to O(1).
     */
    /* Check for integer overflow before malloc */
    if (tile->num_features > SIZE_MAX / sizeof(CTFeature)) {
        return 0;  /* Would overflow */
    }
    CTFeature *all_features = malloc(tile->num_features * sizeof(CTFeature));
    if (!all_features) return 0;

    /* Pointers into the single allocation for each layer */
    CTFeature *layer_features[CT_LAYER_COUNT];
    size_t layer_counts[CT_LAYER_COUNT];
    size_t layer_offsets[CT_LAYER_COUNT];
    memset(layer_counts, 0, sizeof(layer_counts));

    /* Count features per layer */
    for (size_t i = 0; i < tile->num_features; i++) {
        CTLayer layer = tile->features[i].layer;
        if (layer < CT_LAYER_COUNT) {
            layer_counts[layer]++;
        }
    }

    /* Compute offsets into the single allocation */
    size_t offset = 0;
    for (int l = 0; l < CT_LAYER_COUNT; l++) {
        layer_offsets[l] = offset;
        layer_features[l] = (layer_counts[l] > 0) ? &all_features[offset] : NULL;
        offset += layer_counts[l];
        layer_counts[l] = 0;  /* Reset for filling */
    }

    /* Copy features to layer regions */
    for (size_t i = 0; i < tile->num_features; i++) {
        CTLayer layer = tile->features[i].layer;
        if (layer < CT_LAYER_COUNT && layer_features[layer]) {
            layer_features[layer][layer_counts[layer]++] = tile->features[i];
        }
    }

    /* Encode each non-empty layer */
    for (int l = 0; l < CT_LAYER_COUNT; l++) {
        if (layer_counts[l] > 0) {
            encode_layer(&enc, ct_mvt_layer_name((CTLayer)l), opts->extent,
                         layer_features[l], layer_counts[l]);
        }
    }

    /* Single free instead of per-layer frees */
    free(all_features);

    if (enc.error) return 0;
    return enc.offset;
}

size_t ct_generate_mvt(const CTPBFContext *ctx, CTTileCoord coord,
                       const CTMVTOptions *opts,
                       const CTLODConfig *lod,
                       uint8_t *buffer, size_t capacity)
{
    if (!ctx || !buffer) return 0;

    CTMVTOptions default_opts;
    if (!opts) {
        ct_mvt_default_options(&default_opts);
        opts = &default_opts;
    }

    /* Get tile bounds */
    CTBBox bbox = ct_tile_bounds(coord);

    /* Slightly expand for buffer */
    double buf_lat = (bbox.max_lat - bbox.min_lat) * opts->buffer / opts->extent;
    double buf_lon = (bbox.max_lon - bbox.min_lon) * opts->buffer / opts->extent;
    CTBBox expanded_bbox = {
        bbox.min_lat - buf_lat,
        bbox.min_lon - buf_lon,
        bbox.max_lat + buf_lat,
        bbox.max_lon + buf_lon
    };

    /* Get features for this tile */
    CTFeature *raw_features;
    size_t raw_count;
    CTStatus status = ct_pbf_get_bbox_features(ctx, expanded_bbox,
                                               &raw_features, &raw_count);
    if (status != CT_OK || raw_count == 0) {
        free(raw_features);
        return 0;
    }

    /* Convert coordinates from lat/lon to tile pixel coordinates */
    CTTile tile;
    ct_tile_init(&tile, coord);

    int zoom = coord.z;

    for (size_t i = 0; i < raw_count; i++) {
        CTFeature *f = &raw_features[i];

        /* LOD filtering: skip features not visible at this zoom level */
        if (lod && !ct_lod_is_visible(lod, f->layer, f->feature_type,
                                       zoom, f->area_sqm, f->length_m)) {
            free(f->points);
            f->points = NULL;
            continue;
        }

        /* Fast batch coordinate transformation */
        ct_batch_transform_points(coord, opts->extent, f->points, f->num_points);

        /* Clip geometry to tile bounds */
        CTTilePoint *clipped = NULL;
        int clipped_count = 0;

        if (f->type == CT_GEOM_POLYGON) {
            ct_clip_polygon(f->points, f->num_points,
                           opts->extent, opts->buffer,
                           &clipped, &clipped_count);
            /* Skip degenerate polygons (need at least 3 points) */
            if (clipped_count < 3) {
                free(clipped);
                free(f->points);
                f->points = NULL;
                continue;
            }
        } else if (f->type == CT_GEOM_LINESTRING) {
            int *segments = NULL;
            int seg_count = 0;
            ct_clip_linestring(f->points, f->num_points,
                              opts->extent, opts->buffer,
                              &clipped, &clipped_count,
                              &segments, &seg_count);
            free(segments);  /* We only use first segment for now */
            /* Skip degenerate linestrings */
            if (clipped_count < 2) {
                free(clipped);
                free(f->points);
                f->points = NULL;
                continue;
            }
        } else {
            /* Points: just check if within bounds */
            if (f->num_points > 0 &&
                f->points[0].x >= -opts->buffer &&
                f->points[0].x <= opts->extent + opts->buffer &&
                f->points[0].y >= -opts->buffer &&
                f->points[0].y <= opts->extent + opts->buffer) {
                /* Point is in bounds, keep original */
                clipped = f->points;
                clipped_count = f->num_points;
                f->points = NULL;  /* Transfer ownership */
            } else {
                free(f->points);
                f->points = NULL;
                continue;
            }
        }

        /* Replace original points with clipped version */
        if (f->points != clipped) {
            free(f->points);
        }
        f->points = clipped;
        f->num_points = clipped_count;

        /* Simplify geometry if enabled */
        if (opts->simplify && f->num_points > 2) {
            float tolerance = (float)opts->tolerance;
            if (f->type == CT_GEOM_POLYGON) {
                ct_simplify_poly_inplace(f->points, &f->num_points, tolerance);
            } else if (f->type == CT_GEOM_LINESTRING) {
                ct_simplify_line_inplace(f->points, &f->num_points, tolerance);
            }
        }

        /* Skip if simplification made geometry degenerate */
        if ((f->type == CT_GEOM_POLYGON && f->num_points < 3) ||
            (f->type == CT_GEOM_LINESTRING && f->num_points < 2)) {
            free(f->points);
            f->points = NULL;
            continue;
        }

        ct_tile_add_feature(&tile, f);
    }

    /* Encode to MVT */
    size_t mvt_size = ct_encode_mvt(&tile, opts, buffer, capacity);

    /* Cleanup - ct_tile_free handles freeing the points arrays
     * since ct_tile_add_feature took ownership via shallow copy */
    free(raw_features);
    ct_tile_free(&tile);

    return mvt_size;
}

size_t ct_mvt_encoder_finish(CTMVTEncoder *enc)
{
    if (enc->error) return 0;
    return enc->offset;
}
