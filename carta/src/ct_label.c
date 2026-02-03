/*
 * ct_label.c - Map Label Placement
 *
 * Places text labels on map tiles with collision detection.
 * Uses priority sorting to ensure important labels are placed first.
 */

#include "ct_label.h"
#include "ct_pbf.h"
#include "ct_tile.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Default configuration */
#define DEFAULT_PADDING_X     4
#define DEFAULT_PADDING_Y     2
#define DEFAULT_POINT_OFFSET  2
#define DEFAULT_CELL_SIZE     8

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
                size = font_size * 1.4f;
                break;
            case CT_PLACE_CITY:
                size = font_size * 1.2f;
                break;
            case CT_PLACE_TOWN:
                size = font_size * 1.1f;
                break;
            case CT_PLACE_HAMLET:
            case CT_PLACE_LOCALITY:
                size = font_size * 0.9f;
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
