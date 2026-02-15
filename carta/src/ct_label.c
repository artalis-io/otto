/*
 * ct_label.c - Map Label Placement
 *
 * Places text labels on map tiles with collision detection.
 * Uses priority sorting to ensure important labels are placed first.
 */

#include "ct_label.h"
#include "ct_pbf.h"
#include "ct_tile.h"
#include "ct_polylabel.h"
#include "ct_rtree.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Default configuration */
#define DEFAULT_PADDING_X     6
#define DEFAULT_PADDING_Y     4
#define DEFAULT_POINT_OFFSET  4
#define DEFAULT_CELL_SIZE     4

/* Road label minimum zoom levels by road type */
static const int road_label_min_zoom[] = {
    [CT_ROAD_MOTORWAY]    = 8,
    [CT_ROAD_TRUNK]       = 10,
    [CT_ROAD_PRIMARY]     = 12,
    [CT_ROAD_SECONDARY]   = 13,
    [CT_ROAD_TERTIARY]    = 14,
    [CT_ROAD_RESIDENTIAL] = 16,
    [CT_ROAD_SERVICE]     = 17,
    [CT_ROAD_OTHER]       = 17,
};

/* Road label font sizes by road type */
static const float road_label_font_size[] = {
    [CT_ROAD_MOTORWAY]    = 10.0f,
    [CT_ROAD_TRUNK]       = 10.0f,
    [CT_ROAD_PRIMARY]     = 10.0f,
    [CT_ROAD_SECONDARY]   = 9.0f,
    [CT_ROAD_TERTIARY]    = 9.0f,
    [CT_ROAD_RESIDENTIAL] = 8.0f,
    [CT_ROAD_SERVICE]     = 8.0f,
    [CT_ROAD_OTHER]       = 8.0f,
};

/* Road label placement priority by road type */
static const int road_label_priority[] = {
    [CT_ROAD_MOTORWAY]    = 50,
    [CT_ROAD_TRUNK]       = 48,
    [CT_ROAD_PRIMARY]     = 45,
    [CT_ROAD_SECONDARY]   = 40,
    [CT_ROAD_TERTIARY]    = 35,
    [CT_ROAD_RESIDENTIAL] = 20,
    [CT_ROAD_SERVICE]     = 15,
    [CT_ROAD_OTHER]       = 10,
};

/* Maximum angle between consecutive segments for road labels (radians) */
#define ROAD_LABEL_MAX_ANGLE  (30.0f * (float)M_PI / 180.0f)

/* Maximum road labels placed per tile (performance cap) */
#define ROAD_LABEL_MAX_PER_TILE  30

/* ============================================================================
 * Zoom-Adaptive Font Sizing
 * ============================================================================ */

float ct_label_base_font_size(int zoom, int tile_size)
{
    float base;
    if (zoom <= 2) {
        base = 10.0f;
    } else if (zoom <= 5) {
        base = 10.0f;
    } else if (zoom <= 8) {
        base = 12.0f;
    } else if (zoom <= 11) {
        base = 14.0f;
    } else if (zoom <= 14) {
        base = 16.0f;
    } else if (zoom <= 16) {
        base = 14.0f;
    } else {
        base = 12.0f;
    }

    /* Scale proportionally for larger tiles */
    return base * ((float)tile_size / 256.0f);
}

/* ============================================================================
 * Placer Lifecycle
 * ============================================================================ */

CTLabelPlacer *ct_label_placer_create(int tile_width, int tile_height)
{
    if (tile_width <= 0 || tile_height <= 0) {
        return NULL;
    }

    CTLabelPlacer *placer = calloc(1, sizeof(CTLabelPlacer));
    if (!placer) {
        return NULL;
    }

    placer->collision = ct_collision_create(tile_width, tile_height, DEFAULT_CELL_SIZE);
    if (!placer->collision) {
        free(placer);
        return NULL;
    }

    placer->tile_width = tile_width;
    placer->tile_height = tile_height;
    placer->padding_x = DEFAULT_PADDING_X;
    placer->padding_y = DEFAULT_PADDING_Y;
    placer->point_offset = DEFAULT_POINT_OFFSET;

    return placer;
}

void ct_label_placer_free(CTLabelPlacer *placer)
{
    if (!placer) return;

    ct_collision_free(placer->collision);
    free(placer->placements);
    free(placer);
}

void ct_label_placer_reset(CTLabelPlacer *placer)
{
    if (!placer) return;

    ct_collision_reset(placer->collision);
    placer->num_placements = 0;
}

/* ============================================================================
 * Coordinate Conversion
 * ============================================================================ */

void ct_label_geo_to_pixel(CTTileCoord coord,
                           double lat, double lon,
                           int tile_size,
                           int *px, int *py)
{
    if (!px || !py) return;

    /* Get tile bounds */
    CTBBox bbox = ct_tile_bounds(coord);

    /* Calculate relative position within tile [0, 1] */
    double rel_x = (lon - bbox.min_lon) / (bbox.max_lon - bbox.min_lon);
    double rel_y = (bbox.max_lat - lat) / (bbox.max_lat - bbox.min_lat);  /* Y is inverted */

    /* Convert to pixels */
    *px = (int)(rel_x * tile_size);
    *py = (int)(rel_y * tile_size);
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Comparison function for sorting labels by priority (descending) */
static int compare_labels_by_priority(const void *a, const void *b)
{
    const CTLabeledPoint *pa = *(const CTLabeledPoint **)a;
    const CTLabeledPoint *pb = *(const CTLabeledPoint **)b;

    /* Higher priority first */
    if (pa->priority != pb->priority) {
        return pb->priority - pa->priority;
    }

    /* Same priority: prefer larger population */
    if (pa->population != pb->population) {
        return pb->population - pa->population;
    }

    /* Stable sort by ID */
    if (pa->id < pb->id) return -1;
    if (pa->id > pb->id) return 1;
    return 0;
}

/* Add a placement to the placer */
static int add_placement(CTLabelPlacer *placer,
                         const CTLabeledPoint *point,
                         int x, int y, int width, int height,
                         CTLabelAnchor anchor, float font_size)
{
    /* Grow array if needed */
    if (placer->num_placements >= placer->placements_capacity) {
        size_t new_cap = placer->placements_capacity ? placer->placements_capacity * 2 : 64;
        CTLabelPlacement *new_arr = realloc(placer->placements,
                                            new_cap * sizeof(CTLabelPlacement));
        if (!new_arr) return 0;
        placer->placements = new_arr;
        placer->placements_capacity = new_cap;
    }

    CTLabelPlacement *p = &placer->placements[placer->num_placements++];
    p->point = point;
    p->x = x;
    p->y = y;
    p->width = width;
    p->height = height;
    p->anchor = anchor;
    p->font_size = font_size;

    return 1;
}

/* Try to place a label at a specific anchor position */
static int try_anchor(CTLabelPlacer *placer,
                      int point_x, int point_y,
                      int text_width, int text_height,
                      CTLabelAnchor anchor,
                      int *out_x, int *out_y)
{
    int x, y;
    int offset = placer->point_offset;

    switch (anchor) {
        case CT_ANCHOR_RIGHT:
            /* Text to right of point */
            x = point_x + offset;
            y = point_y - text_height / 2;
            break;

        case CT_ANCHOR_LEFT:
            /* Text to left of point */
            x = point_x - text_width - offset;
            y = point_y - text_height / 2;
            break;

        case CT_ANCHOR_TOP:
            /* Text above point */
            x = point_x - text_width / 2;
            y = point_y - text_height - offset;
            break;

        case CT_ANCHOR_BOTTOM:
            /* Text below point */
            x = point_x - text_width / 2;
            y = point_y + offset;
            break;

        case CT_ANCHOR_TOP_RIGHT:
            x = point_x + offset;
            y = point_y - text_height - offset;
            break;

        case CT_ANCHOR_TOP_LEFT:
            x = point_x - text_width - offset;
            y = point_y - text_height - offset;
            break;

        case CT_ANCHOR_BOTTOM_RIGHT:
            x = point_x + offset;
            y = point_y + offset;
            break;

        case CT_ANCHOR_BOTTOM_LEFT:
            x = point_x - text_width - offset;
            y = point_y + offset;
            break;

        case CT_ANCHOR_CENTER:
        default:
            x = point_x - text_width / 2;
            y = point_y - text_height / 2;
            break;
    }

    /* Reject labels that extend past tile boundaries.
     * The adjacent tile will place the same label if the anchor falls
     * within its feature buffer zone and the text fits there. */
    if (x < 0 || y < 0 ||
        x + text_width > placer->tile_width ||
        y + text_height > placer->tile_height) {
        return 0;  /* Would be clipped at tile edge */
    }

    /* Test collision with padding */
    if (ct_collision_test_padded(placer->collision, x, y, text_width, text_height,
                                 placer->padding_x, placer->padding_y)) {
        return 0;  /* Collision */
    }

    *out_x = x;
    *out_y = y;
    return 1;  /* Success */
}

/* Anchor positions to try, in order of preference */
static const CTLabelAnchor anchor_order[] = {
    CT_ANCHOR_RIGHT,         /* Prefer right of point */
    CT_ANCHOR_TOP_RIGHT,
    CT_ANCHOR_BOTTOM_RIGHT,
    CT_ANCHOR_LEFT,
    CT_ANCHOR_TOP_LEFT,
    CT_ANCHOR_BOTTOM_LEFT,
    CT_ANCHOR_TOP,
    CT_ANCHOR_BOTTOM,
    CT_ANCHOR_CENTER
};
static const int num_anchors = sizeof(anchor_order) / sizeof(anchor_order[0]);

/* ============================================================================
 * Label Placement
 * ============================================================================ */

int ct_label_place_single(CTLabelPlacer *placer,
                          const CTLabeledPoint *point,
                          int px, int py,
                          const SHFont *font,
                          float font_size)
{
    if (!placer || !point || !point->name || !font) {
        return 0;
    }

    /* Measure text */
    float text_width_f = sh_font_text_width(font, point->name, font_size);
    float text_height_f = sh_font_line_height(font, font_size);
    int text_width = (int)ceilf(text_width_f);
    int text_height = (int)ceilf(text_height_f);

    if (text_width <= 0 || text_height <= 0) {
        return 0;
    }

    /* Try anchor positions in order of preference */
    for (int i = 0; i < num_anchors; i++) {
        int x, y;
        if (try_anchor(placer, px, py, text_width, text_height,
                       anchor_order[i], &x, &y)) {
            /* Mark as occupied */
            ct_collision_mark_padded(placer->collision, x, y, text_width, text_height,
                                     placer->padding_x, placer->padding_y);

            /* Store placement */
            add_placement(placer, point, x, y, text_width, text_height,
                         anchor_order[i], font_size);

            return 1;
        }
    }

    return 0;  /* Could not place */
}

int ct_label_place_points(CTLabelPlacer *placer,
                          const CTPBFContext *ctx,
                          CTTileCoord coord,
                          const SHFont *font,
                          float font_size)
{
    if (!placer || !ctx || !font) {
        return 0;
    }

    /* Get labels for this tile */
    const CTLabeledPoint **labels = NULL;
    size_t label_count = 0;
    CTStatus status = ct_pbf_get_tile_labels(ctx, coord, &labels, &label_count);
    if (status != CT_OK || label_count == 0) {
        return 0;
    }

    /* Sort by priority (modifies the array) */
    qsort((void *)labels, label_count, sizeof(CTLabeledPoint *), compare_labels_by_priority);

    int placed = 0;

    /* Try to place each label */
    for (size_t i = 0; i < label_count; i++) {
        const CTLabeledPoint *point = labels[i];

        /* Convert geographic to pixel coordinates */
        int px, py;
        ct_label_geo_to_pixel(coord, point->coord.lat, point->coord.lon,
                              placer->tile_width, &px, &py);

        /* Skip if point is too far outside tile */
        if (px < -100 || px > placer->tile_width + 100 ||
            py < -100 || py > placer->tile_height + 100) {
            continue;
        }

        /* Adjust font size based on place type */
        float size = font_size;
        switch (point->type) {
            case CT_PLACE_COUNTRY:
            case CT_PLACE_STATE:
                size = font_size * 1.6f;
                break;
            case CT_PLACE_CITY:
                size = font_size * 1.3f;
                break;
            case CT_PLACE_TOWN:
                size = font_size * 1.0f;
                break;
            case CT_PLACE_VILLAGE:
                size = font_size * 0.85f;
                break;
            case CT_PLACE_HAMLET:
            case CT_PLACE_LOCALITY:
                size = font_size * 0.75f;
                break;
            case CT_PLACE_SUBURB:
            case CT_PLACE_NEIGHBOURHOOD:
                size = font_size * 0.8f;
                break;
            default:
                break;
        }

        /* Try to place */
        if (ct_label_place_single(placer, point, px, py, font, size)) {
            placed++;
        }
    }

    free((void *)labels);
    return placed;
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

void ct_label_set_padding(CTLabelPlacer *placer, int padding_x, int padding_y)
{
    if (!placer) return;
    placer->padding_x = padding_x > 0 ? padding_x : 0;
    placer->padding_y = padding_y > 0 ? padding_y : 0;
}

void ct_label_set_point_offset(CTLabelPlacer *placer, int offset)
{
    if (!placer) return;
    placer->point_offset = offset > 0 ? offset : 0;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

int ct_label_get_count(const CTLabelPlacer *placer)
{
    return placer ? (int)placer->num_placements : 0;
}

float ct_label_get_occupancy(const CTLabelPlacer *placer)
{
    return placer ? ct_collision_get_occupancy(placer->collision) : 0.0f;
}

/* ============================================================================
 * Road Label Placement
 * ============================================================================ */

/* Compare road ways by priority (descending) */
static int compare_roads_by_priority(const void *a, const void *b)
{
    const CTOSMWay *wa = *(const CTOSMWay **)a;
    const CTOSMWay *wb = *(const CTOSMWay **)b;

    int pa = (wa->feature_type >= 0 && wa->feature_type < CT_ROAD_TYPE_COUNT)
             ? road_label_priority[wa->feature_type] : 0;
    int pb = (wb->feature_type >= 0 && wb->feature_type < CT_ROAD_TYPE_COUNT)
             ? road_label_priority[wb->feature_type] : 0;

    if (pa != pb) return pb - pa;

    /* Stable sort by ID */
    if (wa->id < wb->id) return -1;
    if (wa->id > wb->id) return 1;
    return 0;
}

/*
 * Place glyphs along a polyline path.
 * Path must already be oriented left-to-right for readability.
 * Returns the number of glyphs placed, or 0 if the path is too short/curved.
 */
static int place_glyphs_along_path(const float *px, const float *py,
                                    int num_points,
                                    const char *text,
                                    const SHFont *font, float font_size,
                                    CTPathGlyph *glyphs, int max_glyphs)
{
    if (num_points < 2 || !text || !font || !glyphs) return 0;

    /* Calculate total path length */
    float total_length = 0;
    for (int i = 0; i < num_points - 1; i++) {
        float dx = px[i + 1] - px[i];
        float dy = py[i + 1] - py[i];
        total_length += sqrtf(dx * dx + dy * dy);
    }

    /* Measure text width */
    float text_width = sh_font_text_width(font, text, font_size);

    /* Need enough space for text + padding */
    if (total_length < text_width + font_size) return 0;

    /* Center text on path */
    float start_offset = (total_length - text_width) / 2.0f;

    /* Place each glyph along the path, checking curvature per-glyph */
    int glyph_count = 0;
    float cursor = start_offset;
    int seg_idx = 0;
    float seg_dist = 0;
    float prev_angle = -999.0f;

    const char *p = text;
    while (*p && glyph_count < max_glyphs) {
        uint32_t codepoint;
        int len = sh_utf8_decode(p, &codepoint);
        if (len == 0 || codepoint == 0) break;
        p += len;

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            cursor += 0.5f * font_size;
            continue;
        }

        float advance = glyph->advance * font_size;
        float glyph_center = cursor + advance / 2.0f;

        /* Find segment containing this glyph center */
        while (seg_idx < num_points - 2) {
            float dx = px[seg_idx + 1] - px[seg_idx];
            float dy = py[seg_idx + 1] - py[seg_idx];
            float seg_len = sqrtf(dx * dx + dy * dy);
            if (seg_dist + seg_len > glyph_center) break;
            seg_dist += seg_len;
            seg_idx++;
        }

        if (seg_idx >= num_points - 1) break;

        /* Interpolate position on segment */
        float dx = px[seg_idx + 1] - px[seg_idx];
        float dy = py[seg_idx + 1] - py[seg_idx];
        float seg_len = sqrtf(dx * dx + dy * dy);
        if (seg_len < 0.001f) {
            cursor += advance;
            continue;
        }

        float t = (glyph_center - seg_dist) / seg_len;
        if (t < 0) t = 0;
        if (t > 1) t = 1;

        float gx = px[seg_idx] + dx * t;
        float gy = py[seg_idx] + dy * t;
        float angle = atan2f(dy, dx);

        /* Check angle change between consecutive glyphs */
        if (prev_angle > -900.0f) {
            float delta = fabsf(angle - prev_angle);
            if (delta > (float)M_PI) delta = 2.0f * (float)M_PI - delta;
            if (delta > ROAD_LABEL_MAX_ANGLE) return 0;  /* Too curved here */
        }
        prev_angle = angle;

        glyphs[glyph_count].x = gx;
        glyphs[glyph_count].y = gy;
        glyphs[glyph_count].angle = angle;
        glyph_count++;

        cursor += advance;
    }

    return glyph_count;
}

int ct_label_place_roads(CTLabelPlacer *placer,
                         const CTPBFContext *ctx, CTTileCoord coord,
                         const SHFont *font, int tile_size,
                         CTRoadLabelPlacement **out, size_t *out_count)
{
    if (!placer || !ctx || !font || !out || !out_count) {
        if (out) *out = NULL;
        if (out_count) *out_count = 0;
        return 0;
    }

    *out = NULL;
    *out_count = 0;

    /* Get named highway ways for this tile */
    const CTOSMWay **ways = NULL;
    size_t way_count = 0;
    if (ct_pbf_get_tile_named_ways(ctx, coord, &ways, &way_count) != CT_OK || way_count == 0) {
        return 0;
    }

    /* Sort by road type priority */
    qsort((void *)ways, way_count, sizeof(CTOSMWay *), compare_roads_by_priority);

    /* Allocate output */
    size_t capacity = 64;
    CTRoadLabelPlacement *placements = malloc(capacity * sizeof(CTRoadLabelPlacement));
    if (!placements) {
        free((void *)ways);
        return 0;
    }

    CTBBox tile_bbox = ct_tile_bounds(coord);
    int placed = 0;
    int max_labels = ROAD_LABEL_MAX_PER_TILE;

    /* Precompute Mercator transform constants (same for all ways in this tile) */
    double merc_n = (double)(1 << coord.z);
    double merc_lon_scale = merc_n * tile_size / 360.0;
    double merc_lon_offset = 180.0 * merc_lon_scale - (double)coord.x * tile_size;
    double merc_lat_scale = -merc_n * tile_size / (2.0 * M_PI);
    double merc_lat_offset = merc_n * tile_size / 2.0 - (double)coord.y * tile_size;

    /* Scratch buffers for coordinate conversion */
    float *scratch_px = NULL;
    float *scratch_py = NULL;
    size_t scratch_cap = 0;

    /* Scratch for glyphs */
    CTPathGlyph *scratch_glyphs = NULL;
    size_t glyph_cap = 0;

    for (size_t w = 0; w < way_count && placed < max_labels; w++) {
        const CTOSMWay *way = ways[w];

        /* Check min zoom for this road type */
        int road_type = way->feature_type;
        if (road_type < 0 || road_type >= CT_ROAD_TYPE_COUNT) road_type = CT_ROAD_OTHER;
        if (coord.z < road_label_min_zoom[road_type]) continue;

        /* Skip very short ways early (< 3 coords can't form a useful path) */
        if (way->num_coords < 3) continue;

        /* Tile boundary deduplication: only label if midpoint is in tile */
        int mid = way->num_coords / 2;
        if (way->coords[mid].lat < tile_bbox.min_lat ||
            way->coords[mid].lat > tile_bbox.max_lat ||
            way->coords[mid].lon < tile_bbox.min_lon ||
            way->coords[mid].lon > tile_bbox.max_lon) {
            continue;
        }

        /* Convert coords to tile pixels */
        if ((size_t)way->num_coords > scratch_cap) {
            scratch_cap = (size_t)way->num_coords * 2;
            free(scratch_px);
            free(scratch_py);
            scratch_px = malloc(scratch_cap * sizeof(float));
            scratch_py = malloc(scratch_cap * sizeof(float));
            if (!scratch_px || !scratch_py) break;
        }

        /* Convert coords to tile pixels with Mercator projection (float precision) */
        for (int i = 0; i < way->num_coords; i++) {
            double lon = way->coords[i].lon;
            double lat_rad = way->coords[i].lat * M_PI / 180.0;
            double merc_y = log(tan(lat_rad) + 1.0 / cos(lat_rad));

            scratch_px[i] = (float)(lon * merc_lon_scale + merc_lon_offset);
            scratch_py[i] = (float)(merc_y * merc_lat_scale + merc_lat_offset);
        }

        /* Reverse path if it goes right-to-left so text reads naturally */
        if (way->num_coords >= 2 &&
            scratch_px[way->num_coords - 1] < scratch_px[0]) {
            for (int i = 0, j = way->num_coords - 1; i < j; i++, j--) {
                float tmp;
                tmp = scratch_px[i]; scratch_px[i] = scratch_px[j]; scratch_px[j] = tmp;
                tmp = scratch_py[i]; scratch_py[i] = scratch_py[j]; scratch_py[j] = tmp;
            }
        }

        /* Get font size for this road type, scaled for tile size */
        float fsize = road_label_font_size[road_type] * (float)tile_size / 256.0f;

        /* Allocate glyph scratch if needed */
        size_t name_len = strlen(way->name);
        if (name_len > glyph_cap) {
            glyph_cap = name_len * 2;
            free(scratch_glyphs);
            scratch_glyphs = malloc(glyph_cap * sizeof(CTPathGlyph));
            if (!scratch_glyphs) break;
        }

        /* Try to place glyphs along path */
        int num_glyphs = place_glyphs_along_path(
            scratch_px, scratch_py, way->num_coords,
            way->name, font, fsize,
            scratch_glyphs, (int)glyph_cap);

        if (num_glyphs <= 0) continue;

        /* Bounds + collision check: test each glyph AABB */
        float line_height = sh_font_line_height(font, fsize);
        int rejected = 0;
        for (int g = 0; g < num_glyphs; g++) {
            float half_w = fsize * 0.4f;
            float half_h = line_height / 2.0f;
            int gx = (int)(scratch_glyphs[g].x - half_w);
            int gy = (int)(scratch_glyphs[g].y - half_h);
            int gw = (int)(half_w * 2);
            int gh = (int)(half_h * 2);

            /* Reject if any glyph extends past tile boundary */
            if (gx < 0 || gy < 0 ||
                gx + gw > tile_size || gy + gh > tile_size) {
                rejected = 1;
                break;
            }

            if (ct_collision_test_padded(placer->collision, gx, gy, gw, gh,
                                         placer->padding_x, placer->padding_y)) {
                rejected = 1;
                break;
            }
        }

        if (rejected) continue;

        /* Grow output if needed */
        if ((size_t)placed >= capacity) {
            capacity *= 2;
            CTRoadLabelPlacement *grown = realloc(placements, capacity * sizeof(CTRoadLabelPlacement));
            if (!grown) break;
            placements = grown;
        }

        /* Allocate glyph copy BEFORE marking collision grid, so a failed
         * allocation doesn't consume collision space for a label that won't render */
        CTPathGlyph *glyph_copy = malloc(num_glyphs * sizeof(CTPathGlyph));
        if (!glyph_copy) continue;
        memcpy(glyph_copy, scratch_glyphs, num_glyphs * sizeof(CTPathGlyph));

        /* Mark all glyph AABBs in collision grid */
        for (int g = 0; g < num_glyphs; g++) {
            float half_w = fsize * 0.4f;
            float half_h = line_height / 2.0f;
            int gx = (int)(scratch_glyphs[g].x - half_w);
            int gy = (int)(scratch_glyphs[g].y - half_h);
            int gw = (int)(half_w * 2);
            int gh = (int)(half_h * 2);

            ct_collision_mark_padded(placer->collision, gx, gy, gw, gh,
                                     placer->padding_x, placer->padding_y);
        }

        /* Store placement */
        CTRoadLabelPlacement *rp = &placements[placed];
        rp->name = way->name;
        rp->num_glyphs = num_glyphs;
        rp->font_size = fsize;
        rp->priority = road_label_priority[road_type];
        rp->glyphs = glyph_copy;

        placed++;
    }

    free(scratch_px);
    free(scratch_py);
    free(scratch_glyphs);
    free((void *)ways);

    if (placed == 0) {
        free(placements);
        *out = NULL;
        *out_count = 0;
    } else {
        *out = placements;
        *out_count = (size_t)placed;
    }

    return placed;
}

void ct_label_road_placements_free(CTRoadLabelPlacement *p, size_t count)
{
    if (!p) return;
    for (size_t i = 0; i < count; i++) {
        free(p[i].glyphs);
    }
    free(p);
}

/* ============================================================================
 * Area Label Placement (stub - requires polylabel)
 * ============================================================================ */

/* Maximum area labels per tile and R-tree candidates */
#define AREA_LABEL_MAX_PER_TILE    20
#define AREA_LABEL_MAX_CANDIDATES  512

int ct_label_place_areas(CTLabelPlacer *placer,
                         const CTPBFContext *ctx,
                         CTTileCoord coord,
                         const SHFont *font,
                         float font_size)
{
    if (!placer || !ctx || !font) return 0;

    /* Only show at reasonable zoom levels */
    if (coord.z < 10) return 0;

    int placed = 0;
    CTBBox tile_bbox = ct_tile_bounds(coord);

    /* Use MP R-tree to find candidate multipolygons instead of scanning all 75K+ */
    if (!ctx->mp_rtree || ctx->num_multipolygons == 0) return 0;

    uint32_t *candidates = malloc(AREA_LABEL_MAX_CANDIDATES * sizeof(uint32_t));
    if (!candidates) return 0;

    size_t num_candidates = ct_rtree_query(ctx->mp_rtree, tile_bbox, candidates,
                                            AREA_LABEL_MAX_CANDIDATES);

    /* Polylabel precision: 0.01 degrees (~1km) is plenty for label placement.
     * Was 0.001 (~100m) which makes polylabel iterate much deeper. */
    double polylabel_precision = 0.01;

    for (size_t ci = 0; ci < num_candidates && placed < AREA_LABEL_MAX_PER_TILE; ci++) {
        uint32_t mp_idx = candidates[ci];
        if (mp_idx >= ctx->num_multipolygons) continue;

        const CTAssembledMultipolygon *mp = &ctx->multipolygons[mp_idx];

        if (!mp->name || mp->name[0] == '\0') continue;
        if (mp->num_rings < 1) continue;

        /* Find pole of inaccessibility using polylabel */
        double pole_x = 0, pole_y = 0, pole_dist = 0;

        /* Build ring arrays for polylabel */
        const CTCoord **rings = malloc(mp->num_rings * sizeof(CTCoord *));
        int *ring_sizes = malloc(mp->num_rings * sizeof(int));
        if (!rings || !ring_sizes) {
            free(rings);
            free(ring_sizes);
            continue;
        }

        for (int r = 0; r < mp->num_rings; r++) {
            rings[r] = mp->rings[r].coords;
            ring_sizes[r] = mp->rings[r].num_coords;
        }

        int found = ct_polylabel_with_holes(rings, ring_sizes, mp->num_rings,
                                             polylabel_precision, &pole_x, &pole_y, &pole_dist);
        free(rings);
        free(ring_sizes);

        if (!found) continue;

        /* Check if pole is in tile bbox */
        if (pole_x < tile_bbox.min_lon || pole_x > tile_bbox.max_lon ||
            pole_y < tile_bbox.min_lat || pole_y > tile_bbox.max_lat) {
            continue;
        }

        /* Check if text fits inside polygon */
        float area_size = font_size * 0.85f;
        float text_width = sh_font_text_width(font, mp->name, area_size);

        /* pole_dist is in degrees; convert to pixels for comparison */
        double deg_per_pixel = (tile_bbox.max_lon - tile_bbox.min_lon) / (double)placer->tile_width;
        if (deg_per_pixel > 0) {
            double pole_dist_px = pole_dist / deg_per_pixel;
            if (pole_dist_px * 2.0 < text_width) continue;
        }

        /* Convert to pixel coordinates */
        int px, py;
        ct_label_geo_to_pixel(coord, pole_y, pole_x, placer->tile_width, &px, &py);

        /* Skip if outside tile */
        if (px < -50 || px > placer->tile_width + 50 ||
            py < -50 || py > placer->tile_height + 50) {
            continue;
        }

        /* Create a temporary labeled point for placement */
        CTLabeledPoint area_point;
        area_point.id = 0;
        area_point.coord.lat = pole_y;
        area_point.coord.lon = pole_x;
        area_point.type = CT_PLACE_UNKNOWN;
        area_point.name = mp->name;
        area_point.population = 0;
        area_point.min_zoom = 10;
        area_point.priority = 5;

        if (ct_label_place_single(placer, &area_point, px, py, font, area_size)) {
            placed++;
        }
    }

    free(candidates);
    return placed;
}
