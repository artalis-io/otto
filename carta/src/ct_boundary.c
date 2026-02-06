/*
 * ct_boundary.c - Boundary relation assembly
 *
 * Stitches way segments from boundary relations into continuous linestrings.
 */

#include "ct_boundary.h"
#include "ct_rtree.h"
#include "sh_hashmap.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Tolerance for endpoint matching (in degrees, ~1m at equator) */
#define COORD_EPSILON 0.00001

/* Earth radius for length calculation */
#define EARTH_RADIUS_M 6371000.0
#define DEG_TO_RAD(d) ((d) * 3.14159265358979323846 / 180.0)

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static int coords_equal(const CTCoord *a, const CTCoord *b)
{
    return fabs(a->lat - b->lat) < COORD_EPSILON &&
           fabs(a->lon - b->lon) < COORD_EPSILON;
}

/*
 * Compute bounding box of coordinates.
 */
static void compute_bbox(const CTCoord *coords, int count, CTBBox *bbox)
{
    if (count == 0) {
        bbox->min_lat = bbox->max_lat = 0;
        bbox->min_lon = bbox->max_lon = 0;
        return;
    }

    bbox->min_lat = bbox->max_lat = coords[0].lat;
    bbox->min_lon = bbox->max_lon = coords[0].lon;

    for (int i = 1; i < count; i++) {
        if (coords[i].lat < bbox->min_lat) bbox->min_lat = coords[i].lat;
        if (coords[i].lat > bbox->max_lat) bbox->max_lat = coords[i].lat;
        if (coords[i].lon < bbox->min_lon) bbox->min_lon = coords[i].lon;
        if (coords[i].lon > bbox->max_lon) bbox->max_lon = coords[i].lon;
    }
}

/*
 * Compute total length in meters using Haversine formula.
 */
static float compute_length_m(const CTCoord *coords, int count)
{
    if (count < 2) return 0.0f;

    double total = 0.0;
    for (int i = 0; i < count - 1; i++) {
        double lat1 = DEG_TO_RAD(coords[i].lat);
        double lon1 = DEG_TO_RAD(coords[i].lon);
        double lat2 = DEG_TO_RAD(coords[i + 1].lat);
        double lon2 = DEG_TO_RAD(coords[i + 1].lon);

        double dlat = lat2 - lat1;
        double dlon = lon2 - lon1;

        double a = sin(dlat / 2) * sin(dlat / 2) +
                   cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));

        total += EARTH_RADIUS_M * c;
    }
    return (float)total;
}

/* ============================================================================
 * Way Segment for Stitching
 * ============================================================================ */

typedef struct {
    CTCoord *coords;
    int num_coords;
    int used;       /* 1 if already consumed in a linestring */
} WaySegment;

/*
 * Find a way segment that can continue from 'endpoint'.
 * Returns segment index, or -1 if not found.
 * Sets *reverse = 1 if segment should be reversed.
 */
static int find_connecting_segment(const WaySegment *segments, int num_segments,
                                   const CTCoord *endpoint, int *reverse)
{
    for (int i = 0; i < num_segments; i++) {
        if (segments[i].used || segments[i].num_coords < 2) continue;

        /* Check if first point matches */
        if (coords_equal(&segments[i].coords[0], endpoint)) {
            *reverse = 0;
            return i;
        }

        /* Check if last point matches (need to reverse) */
        if (coords_equal(&segments[i].coords[segments[i].num_coords - 1], endpoint)) {
            *reverse = 1;
            return i;
        }
    }
    return -1;
}

/*
 * Append way coordinates to a linestring, optionally reversed.
 * Skip first point if skip_first is set (to avoid duplicate at join).
 */
static int append_coords(CTCoord **line, int *line_count, int *line_capacity,
                         const CTCoord *coords, int count, int reverse, int skip_first)
{
    int to_add = count - (skip_first ? 1 : 0);
    if (to_add <= 0) return 1;

    while (*line_count + to_add > *line_capacity) {
        int new_cap = *line_capacity ? *line_capacity * 2 : 256;
        CTCoord *new_line = realloc(*line, new_cap * sizeof(CTCoord));
        if (!new_line) return 0;
        *line = new_line;
        *line_capacity = new_cap;
    }

    if (!reverse) {
        int start = skip_first ? 1 : 0;
        for (int i = start; i < count; i++) {
            (*line)[(*line_count)++] = coords[i];
        }
    } else {
        int end = skip_first ? count - 2 : count - 1;
        for (int i = end; i >= 0; i--) {
            (*line)[(*line_count)++] = coords[i];
        }
    }

    return 1;
}

/*
 * Build a linestring starting from a given segment.
 * Extends in both directions until no more connections.
 * Returns allocated coordinate array, or NULL on failure.
 */
static CTCoord *build_linestring(WaySegment *segments, int num_segments,
                                 int start_idx, int *coord_count_out)
{
    CTCoord *line = NULL;
    int line_count = 0;
    int line_capacity = 0;

    WaySegment *start = &segments[start_idx];
    start->used = 1;

    /* Add initial segment */
    if (!append_coords(&line, &line_count, &line_capacity,
                       start->coords, start->num_coords, 0, 0)) {
        free(line);
        return NULL;
    }

    /* Extend forward from end of line */
    for (;;) {
        int reverse;
        int idx = find_connecting_segment(segments, num_segments,
                                          &line[line_count - 1], &reverse);
        if (idx < 0) break;

        segments[idx].used = 1;
        if (!append_coords(&line, &line_count, &line_capacity,
                           segments[idx].coords, segments[idx].num_coords,
                           reverse, 1)) {
            free(line);
            return NULL;
        }
    }

    /* Extend backward from start of line */
    for (;;) {
        int reverse;
        int idx = find_connecting_segment(segments, num_segments,
                                          &line[0], &reverse);
        if (idx < 0) break;

        segments[idx].used = 1;

        /* Prepend by building temp array and swapping */
        CTCoord *prepend = NULL;
        int prepend_count = 0;
        int prepend_cap = 0;

        if (!append_coords(&prepend, &prepend_count, &prepend_cap,
                           segments[idx].coords, segments[idx].num_coords,
                           !reverse, 0)) {  /* Reverse sense for prepending */
            free(line);
            return NULL;
        }

        /* Append existing line (skip first point - it's the join) */
        if (!append_coords(&prepend, &prepend_count, &prepend_cap,
                           line, line_count, 0, 1)) {
            free(prepend);
            free(line);
            return NULL;
        }

        free(line);
        line = prepend;
        line_count = prepend_count;
        line_capacity = prepend_cap;
    }

    *coord_count_out = line_count;
    return line;
}

/* ============================================================================
 * Boundary Assembly
 * ============================================================================ */

/*
 * Assemble a single boundary from a relation.
 * May produce multiple disconnected linestrings.
 */
static CTStatus assemble_one_boundary(CTPBFContext *ctx,
                                      const CTOSMRelation *rel,
                                      CTAssembledBoundary **out_boundaries,
                                      int *out_count)
{
    *out_boundaries = NULL;
    *out_count = 0;

    if (rel->num_members == 0) return CT_OK;

    /* Collect way segments */
    WaySegment *segments = calloc(rel->num_members, sizeof(WaySegment));
    if (!segments) return CT_ERROR_OUT_OF_MEMORY;

    int num_segments = 0;
    for (int i = 0; i < rel->num_members; i++) {
        if (rel->members[i].type != CT_MEMBER_WAY) continue;

        int64_t way_id = rel->members[i].ref;
        size_t way_idx = sh_hashmap_i64_lookup(ctx->way_map, way_id);
        if (way_idx == SIZE_MAX) continue;

        CTOSMWay *way = &ctx->ways[way_idx];
        if (way->num_coords < 2) continue;

        segments[num_segments].coords = way->coords;
        segments[num_segments].num_coords = way->num_coords;
        segments[num_segments].used = 0;
        num_segments++;
    }

    if (num_segments == 0) {
        free(segments);
        return CT_OK;
    }

    /* Build linestrings from unused segments */
    CTAssembledBoundary *boundaries = NULL;
    int boundary_count = 0;
    int boundary_capacity = 0;

    for (int i = 0; i < num_segments; i++) {
        if (segments[i].used) continue;

        int coord_count;
        CTCoord *coords = build_linestring(segments, num_segments, i, &coord_count);
        if (!coords) continue;

        /* Grow boundaries array if needed */
        if (boundary_count >= boundary_capacity) {
            int new_cap = boundary_capacity ? boundary_capacity * 2 : 4;
            CTAssembledBoundary *new_b = realloc(boundaries, new_cap * sizeof(CTAssembledBoundary));
            if (!new_b) {
                free(coords);
                /* Clean up partial results */
                for (int j = 0; j < boundary_count; j++) {
                    free(boundaries[j].coords);
                    free(boundaries[j].name);
                }
                free(boundaries);
                free(segments);
                return CT_ERROR_OUT_OF_MEMORY;
            }
            boundaries = new_b;
            boundary_capacity = new_cap;
        }

        CTAssembledBoundary *b = &boundaries[boundary_count++];
        b->relation_id = rel->id;
        b->coords = coords;
        b->num_coords = coord_count;
        compute_bbox(coords, coord_count, &b->bbox);
        b->length_m = compute_length_m(coords, coord_count);
        b->name = rel->name ? strdup(rel->name) : NULL;

        /* Extract boundary type and admin level from feature_type encoding */
        int feature_type = rel->feature_type;
        if ((feature_type & 0xFF) == CT_BOUNDARY_TYPE_PROTECTED) {
            b->boundary_type = CT_BOUNDARY_TYPE_PROTECTED;
            b->admin_level = 0;  /* N/A for protected areas */
        } else {
            b->boundary_type = CT_BOUNDARY_TYPE_ADMIN;
            b->admin_level = (feature_type >> 8) & 0xFF;
            if (b->admin_level == 0) b->admin_level = CT_BOUNDARY_OTHER;
        }
    }

    free(segments);

    *out_boundaries = boundaries;
    *out_count = boundary_count;
    return CT_OK;
}

CTStatus ct_assemble_boundaries(CTPBFContext *ctx)
{
    if (!ctx || !ctx->way_map) return CT_ERROR_INVALID_ARGUMENT;

    /* Process each boundary relation */
    for (size_t i = 0; i < ctx->num_boundary_relations; i++) {
        CTOSMRelation *rel = &ctx->boundary_relations[i];

        CTAssembledBoundary *boundaries;
        int count;
        CTStatus status = assemble_one_boundary(ctx, rel, &boundaries, &count);
        if (status != CT_OK) return status;

        /* Add to context */
        for (int j = 0; j < count; j++) {
            if (ctx->num_boundaries >= ctx->boundaries_capacity) {
                size_t new_cap = ctx->boundaries_capacity ? ctx->boundaries_capacity * 2 : 100;
                CTAssembledBoundary *new_b = realloc(ctx->boundaries, new_cap * sizeof(CTAssembledBoundary));
                if (!new_b) {
                    /* Clean up remaining boundaries */
                    for (int k = j; k < count; k++) {
                        free(boundaries[k].coords);
                        free(boundaries[k].name);
                    }
                    free(boundaries);
                    return CT_ERROR_OUT_OF_MEMORY;
                }
                ctx->boundaries = new_b;
                ctx->boundaries_capacity = new_cap;
            }
            ctx->boundaries[ctx->num_boundaries++] = boundaries[j];
        }
        free(boundaries);  /* Array only, contents moved to ctx */
    }

    return CT_OK;
}

/* ============================================================================
 * Boundary R-Tree
 * ============================================================================ */

CTStatus ct_build_boundary_rtree(CTPBFContext *ctx)
{
    if (!ctx || ctx->num_boundaries == 0) return CT_OK;

    /* Collect bounding boxes */
    CTBBox *bboxes = malloc(ctx->num_boundaries * sizeof(CTBBox));
    if (!bboxes) return CT_ERROR_OUT_OF_MEMORY;

    CTBBox data_bbox = {90, 180, -90, -180};
    for (size_t i = 0; i < ctx->num_boundaries; i++) {
        bboxes[i] = ctx->boundaries[i].bbox;
        if (bboxes[i].min_lat < data_bbox.min_lat) data_bbox.min_lat = bboxes[i].min_lat;
        if (bboxes[i].min_lon < data_bbox.min_lon) data_bbox.min_lon = bboxes[i].min_lon;
        if (bboxes[i].max_lat > data_bbox.max_lat) data_bbox.max_lat = bboxes[i].max_lat;
        if (bboxes[i].max_lon > data_bbox.max_lon) data_bbox.max_lon = bboxes[i].max_lon;
    }

    /* Build R-tree from bboxes */
    ctx->boundary_rtree = ct_rtree_build_from_bboxes(bboxes, ctx->num_boundaries, data_bbox);
    free(bboxes);

    if (!ctx->boundary_rtree) return CT_ERROR_OUT_OF_MEMORY;

    return CT_OK;
}

CTStatus ct_boundary_query(const CTPBFContext *ctx, CTBBox bbox,
                           size_t **indices_out, size_t *count_out)
{
    *indices_out = NULL;
    *count_out = 0;

    if (!ctx || !ctx->boundary_rtree) return CT_OK;

    /* Allocate results buffer (generous max) */
    size_t max_results = ctx->num_boundaries < 10000 ? ctx->num_boundaries : 10000;
    uint32_t *indices = malloc(max_results * sizeof(uint32_t));
    if (!indices) return CT_ERROR_OUT_OF_MEMORY;

    /* Query R-tree */
    size_t count = ct_rtree_query(ctx->boundary_rtree, bbox, indices, max_results);

    /* Convert to size_t array */
    if (count > 0) {
        size_t *result = malloc(count * sizeof(size_t));
        if (!result) {
            free(indices);
            return CT_ERROR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < count; i++) {
            result[i] = indices[i];
        }
        *indices_out = result;
        *count_out = count;
    }

    free(indices);
    return CT_OK;
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

void ct_boundary_config_init(CTBoundaryConfig *config)
{
    config->min_admin_level = 2;   /* Country borders */
    config->max_admin_level = 6;   /* Down to county level */
    config->include_protected_areas = 1;  /* Include national parks */
}

void ct_pbf_set_boundary_config(CTPBFContext *ctx, const CTBoundaryConfig *config)
{
    if (!ctx || !config) return;
    ctx->boundary_config = *config;
}
