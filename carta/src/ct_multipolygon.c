/*
 * ct_multipolygon.c - Multipolygon assembly from OSM relations
 */

#include "ct_multipolygon.h"
#include "ct_rtree.h"
#include "shared.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Tolerance for endpoint matching (in degrees, ~1m at equator) */
#define COORD_EPSILON 0.00001

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static int coords_equal(const CTCoord *a, const CTCoord *b)
{
    return fabs(a->lat - b->lat) < COORD_EPSILON &&
           fabs(a->lon - b->lon) < COORD_EPSILON;
}

/*
 * Compute signed area of a ring using the Shoelace formula.
 * Positive = counter-clockwise (outer), negative = clockwise (inner/hole).
 */
static double signed_area(const CTCoord *coords, int count)
{
    if (count < 3) return 0.0;

    double area = 0.0;
    for (int i = 0; i < count; i++) {
        int j = (i + 1) % count;
        area += coords[i].lon * coords[j].lat;
        area -= coords[j].lon * coords[i].lat;
    }
    return area / 2.0;
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
 * Compute rough area in square meters from lat/lon coordinates.
 */
static float estimate_area_sqm(const CTCoord *coords, int count)
{
    if (count < 3) return 0.0f;

    /* Use a simple approximation: treat as planar at the centroid latitude */
    double centroid_lat = 0;
    for (int i = 0; i < count; i++) {
        centroid_lat += coords[i].lat;
    }
    centroid_lat /= count;

    /* Meters per degree at this latitude */
    double lat_scale = 111320.0;  /* m/degree latitude */
    double lon_scale = 111320.0 * cos(centroid_lat * 3.14159265358979 / 180.0);

    /* Shoelace formula in scaled coordinates */
    double area = 0.0;
    for (int i = 0; i < count; i++) {
        int j = (i + 1) % count;
        double x1 = coords[i].lon * lon_scale;
        double y1 = coords[i].lat * lat_scale;
        double x2 = coords[j].lon * lon_scale;
        double y2 = coords[j].lat * lat_scale;
        area += x1 * y2 - x2 * y1;
    }
    return (float)fabs(area / 2.0);
}

const char *ct_get_role_string(const CTPBFContext *ctx, uint32_t role_idx)
{
    if (!ctx || role_idx == 0) return "";
    /* Check bounds without subtraction to avoid uint32_t wrap-around */
    if (role_idx > ctx->num_role_strings) return "";
    /* role_strings[0] is NULL (placeholder for empty role) */
    const char *role = ctx->role_strings[role_idx - 1];
    return role ? role : "";
}

/* ============================================================================
 * Ring Assembly
 * ============================================================================ */

/* A way segment for ring building */
typedef struct {
    const CTOSMWay *way;
    int reversed;     /* 1 if should use way coords in reverse order */
    int used;         /* 1 if already consumed in a ring */
} WaySegment;

/*
 * Find a way segment that can continue the ring from 'endpoint'.
 * Returns segment index, or -1 if none found.
 */
static int find_connecting_segment(WaySegment *segments, int num_segments,
                                   const CTCoord *endpoint, int *reverse_out)
{
    for (int i = 0; i < num_segments; i++) {
        if (segments[i].used) continue;
        const CTOSMWay *way = segments[i].way;
        if (way->num_coords < 2) continue;

        /* Check if way's start connects */
        if (coords_equal(endpoint, &way->coords[0])) {
            *reverse_out = 0;
            return i;
        }
        /* Check if way's end connects (need to reverse) */
        if (coords_equal(endpoint, &way->coords[way->num_coords - 1])) {
            *reverse_out = 1;
            return i;
        }
    }
    return -1;
}

/*
 * Append way coordinates to a ring, optionally reversed.
 * skip_first: skip the first coordinate (it's the connecting point)
 */
static int append_way_coords(CTCoord **ring, int *ring_count, int *ring_capacity,
                             const CTOSMWay *way, int reversed, int skip_first)
{
    int start = skip_first ? 1 : 0;
    int to_add = way->num_coords - start;

    /* Grow buffer if needed */
    while (*ring_count + to_add > *ring_capacity) {
        int new_cap = *ring_capacity ? *ring_capacity * 2 : 256;
        CTCoord *new_ring = realloc(*ring, new_cap * sizeof(CTCoord));
        if (!new_ring) return 0;
        *ring = new_ring;
        *ring_capacity = new_cap;
    }

    if (reversed) {
        int end_idx = skip_first ? way->num_coords - 2 : way->num_coords - 1;
        for (int i = end_idx; i >= 0; i--) {
            (*ring)[(*ring_count)++] = way->coords[i];
        }
    } else {
        for (int i = start; i < way->num_coords; i++) {
            (*ring)[(*ring_count)++] = way->coords[i];
        }
    }
    return 1;
}

/*
 * Build a single closed ring starting from a given segment.
 * Returns coordinates and count, or NULL on failure.
 */
static CTCoord *build_ring(WaySegment *segments, int num_segments,
                           int start_idx, int *coord_count_out)
{
    CTCoord *ring = NULL;
    int ring_count = 0;
    int ring_capacity = 0;

    /* Start with the first segment */
    segments[start_idx].used = 1;
    const CTOSMWay *start_way = segments[start_idx].way;

    if (!append_way_coords(&ring, &ring_count, &ring_capacity, start_way, 0, 0)) {
        free(ring);
        return NULL;
    }

    /* Check if already closed */
    if (ring_count >= 3 && coords_equal(&ring[0], &ring[ring_count - 1])) {
        *coord_count_out = ring_count;
        return ring;
    }

    /* Keep adding segments until we close the ring */
    int max_iterations = num_segments;
    while (max_iterations-- > 0) {
        CTCoord *endpoint = &ring[ring_count - 1];
        int reverse;
        int next_idx = find_connecting_segment(segments, num_segments, endpoint, &reverse);

        if (next_idx < 0) {
            /* No connecting segment found - incomplete ring */
            break;
        }

        segments[next_idx].used = 1;
        if (!append_way_coords(&ring, &ring_count, &ring_capacity,
                               segments[next_idx].way, reverse, 1)) {
            free(ring);
            return NULL;
        }

        /* Check if closed */
        if (coords_equal(&ring[0], &ring[ring_count - 1])) {
            *coord_count_out = ring_count;
            return ring;
        }
    }

    /* Could not close the ring */
    free(ring);
    *coord_count_out = 0;
    return NULL;
}

/* ============================================================================
 * Multipolygon Assembly
 * ============================================================================ */

/* Hash function - must match hash_id() in ct_pbf.c */
static uint64_t hash_id(int64_t id)
{
    uint64_t x = (uint64_t)id;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* Lookup way by ID using way_map */
static const CTOSMWay *lookup_way(const CTPBFContext *ctx, int64_t id)
{
    if (!ctx->way_map.keys || ctx->way_map.count == 0) return NULL;

    size_t cap = ctx->way_map.capacity;
    uint64_t h = hash_id(id) % cap;

    while (ctx->way_map.keys[h] != 0) {
        if (ctx->way_map.keys[h] == id) {
            uint32_t way_idx = ctx->way_map.values[h];
            if (way_idx < ctx->num_ways) {
                return &ctx->ways[way_idx];
            }
            return NULL;
        }
        h = (h + 1) % cap;
    }
    return NULL;
}

/* Initial capacity for WaySegment scratch buffers */
#define MP_SCRATCH_INITIAL_CAPACITY 64

/*
 * Ensure scratch buffer has enough capacity, growing if needed.
 * Returns 0 on allocation failure, 1 on success.
 */
static int ensure_scratch_capacity(void **buffer, size_t *capacity,
                                   size_t needed, size_t elem_size)
{
    if (needed <= *capacity) return 1;

    size_t new_cap = *capacity ? *capacity : MP_SCRATCH_INITIAL_CAPACITY;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    void *new_buf = realloc(*buffer, new_cap * elem_size);
    if (!new_buf) return 0;

    *buffer = new_buf;
    *capacity = new_cap;
    return 1;
}

/*
 * Assemble a single multipolygon from a relation.
 */
static CTStatus assemble_one_multipolygon(CTPBFContext *ctx,
                                          const CTOSMRelation *rel,
                                          CTAssembledMultipolygon *mp)
{
    /* Use pre-allocated scratch buffers from context (eliminates per-relation malloc) */
    WaySegment *outer_segs = (WaySegment *)ctx->mp_scratch.outer_segs;
    WaySegment *inner_segs = (WaySegment *)ctx->mp_scratch.inner_segs;
    int num_outer = 0, num_inner = 0;

    for (int i = 0; i < rel->num_members; i++) {
        if (rel->members[i].type != CT_MEMBER_WAY) continue;

        const CTOSMWay *way = lookup_way(ctx, rel->members[i].ref);
        if (!way || way->num_coords < 2) continue;

        const char *role = ct_get_role_string(ctx, rel->members[i].role_idx);
        int is_outer = (strcmp(role, "outer") == 0 || strlen(role) == 0);
        int is_inner = (strcmp(role, "inner") == 0);

        if (is_outer) {
            /* Grow scratch buffer if needed */
            if (!ensure_scratch_capacity(&ctx->mp_scratch.outer_segs,
                                         &ctx->mp_scratch.outer_capacity,
                                         (size_t)(num_outer + 1), sizeof(WaySegment))) {
                return CT_ERROR_OUT_OF_MEMORY;
            }
            outer_segs = (WaySegment *)ctx->mp_scratch.outer_segs;
            outer_segs[num_outer].way = way;
            outer_segs[num_outer].reversed = 0;
            outer_segs[num_outer].used = 0;
            num_outer++;
        } else if (is_inner) {
            /* Grow scratch buffer if needed */
            if (!ensure_scratch_capacity(&ctx->mp_scratch.inner_segs,
                                         &ctx->mp_scratch.inner_capacity,
                                         (size_t)(num_inner + 1), sizeof(WaySegment))) {
                return CT_ERROR_OUT_OF_MEMORY;
            }
            inner_segs = (WaySegment *)ctx->mp_scratch.inner_segs;
            inner_segs[num_inner].way = way;
            inner_segs[num_inner].reversed = 0;
            inner_segs[num_inner].used = 0;
            num_inner++;
        }
    }

    if (num_outer == 0) {
        /* No outer rings - skip this relation (scratch buffers persist for next relation) */
        return CT_OK;
    }

    /* Build all rings */
    CTMultipolygonRing *rings = NULL;
    int ring_count = 0;
    int ring_cap = 0;

    /* Build outer rings */
    for (int i = 0; i < num_outer; i++) {
        if (outer_segs[i].used) continue;

        int coord_count;
        CTCoord *coords = build_ring(outer_segs, num_outer, i, &coord_count);
        if (!coords || coord_count < 4) {
            free(coords);
            continue;
        }

        /* Ensure outer ring is CCW (positive area) */
        double area = signed_area(coords, coord_count);
        if (area < 0) {
            /* Reverse to make CCW */
            for (int j = 0; j < coord_count / 2; j++) {
                CTCoord tmp = coords[j];
                coords[j] = coords[coord_count - 1 - j];
                coords[coord_count - 1 - j] = tmp;
            }
        }

        if (ring_count >= ring_cap) {
            int new_cap = ring_cap ? ring_cap * 2 : 4;
            CTMultipolygonRing *new_rings = realloc(rings, new_cap * sizeof(CTMultipolygonRing));
            if (!new_rings) {
                free(coords);
                goto error_rings;
            }
            rings = new_rings;
            ring_cap = new_cap;
        }

        rings[ring_count].coords = coords;
        rings[ring_count].num_coords = coord_count;
        rings[ring_count].is_outer = 1;
        ring_count++;
    }

    /* Build inner rings (holes) */
    for (int i = 0; i < num_inner; i++) {
        if (inner_segs[i].used) continue;

        int coord_count;
        CTCoord *coords = build_ring(inner_segs, num_inner, i, &coord_count);
        if (!coords || coord_count < 4) {
            free(coords);
            continue;
        }

        /* Ensure inner ring is CW (negative area) */
        double area = signed_area(coords, coord_count);
        if (area > 0) {
            /* Reverse to make CW */
            for (int j = 0; j < coord_count / 2; j++) {
                CTCoord tmp = coords[j];
                coords[j] = coords[coord_count - 1 - j];
                coords[coord_count - 1 - j] = tmp;
            }
        }

        if (ring_count >= ring_cap) {
            int new_cap = ring_cap ? ring_cap * 2 : 4;
            CTMultipolygonRing *new_rings = realloc(rings, new_cap * sizeof(CTMultipolygonRing));
            if (!new_rings) {
                free(coords);
                goto error_rings;
            }
            rings = new_rings;
            ring_cap = new_cap;
        }

        rings[ring_count].coords = coords;
        rings[ring_count].num_coords = coord_count;
        rings[ring_count].is_outer = 0;
        ring_count++;
    }

    /* Scratch buffers persist in context for reuse - don't free here */

    if (ring_count == 0) {
        free(rings);
        return CT_OK;
    }

    /* Compute overall bbox from outer rings */
    CTBBox bbox = { 90, 180, -90, -180 };
    float total_area = 0;
    for (int i = 0; i < ring_count; i++) {
        if (rings[i].is_outer) {
            CTBBox ring_bbox;
            compute_bbox(rings[i].coords, rings[i].num_coords, &ring_bbox);
            if (ring_bbox.min_lat < bbox.min_lat) bbox.min_lat = ring_bbox.min_lat;
            if (ring_bbox.max_lat > bbox.max_lat) bbox.max_lat = ring_bbox.max_lat;
            if (ring_bbox.min_lon < bbox.min_lon) bbox.min_lon = ring_bbox.min_lon;
            if (ring_bbox.max_lon > bbox.max_lon) bbox.max_lon = ring_bbox.max_lon;
            total_area += estimate_area_sqm(rings[i].coords, rings[i].num_coords);
        }
    }

    /* Store result */
    mp->rings = rings;
    mp->num_rings = ring_count;
    mp->bbox = bbox;
    mp->area_sqm = total_area;
    mp->feature_class = rel->feature_class;
    mp->feature_type = rel->feature_type;
    mp->name = rel->name ? strdup(rel->name) : NULL;

    return CT_OK;

error_rings:
    for (int i = 0; i < ring_count; i++) {
        free(rings[i].coords);
    }
    free(rings);
    /* Scratch buffers persist in context - don't free on error */
    return CT_ERROR_OUT_OF_MEMORY;
}

CTStatus ct_assemble_multipolygons(CTPBFContext *ctx)
{
    if (!ctx) return CT_ERROR_INVALID_ARGUMENT;

    for (size_t i = 0; i < ctx->num_relations; i++) {
        const CTOSMRelation *rel = &ctx->relations[i];
        if (!rel->is_multipolygon) continue;

        /* Grow multipolygon array if needed */
        if (ctx->num_multipolygons >= ctx->multipolygons_capacity) {
            size_t new_cap = ctx->multipolygons_capacity ? ctx->multipolygons_capacity * 2 : 100;
            CTAssembledMultipolygon *new_mps = realloc(ctx->multipolygons,
                                                        new_cap * sizeof(CTAssembledMultipolygon));
            if (!new_mps) return CT_ERROR_OUT_OF_MEMORY;
            ctx->multipolygons = new_mps;
            ctx->multipolygons_capacity = new_cap;
        }

        CTAssembledMultipolygon *mp = &ctx->multipolygons[ctx->num_multipolygons];
        memset(mp, 0, sizeof(*mp));

        CTStatus status = assemble_one_multipolygon(ctx, rel, mp);
        if (status != CT_OK) return status;

        if (mp->num_rings > 0) {
            ctx->num_multipolygons++;
            ctx->multipolygons_assembled++;
        }
    }

    return CT_OK;
}

CTStatus ct_build_multipolygon_rtree(CTPBFContext *ctx)
{
    if (!ctx) return CT_ERROR_INVALID_ARGUMENT;
    if (ctx->num_multipolygons == 0) return CT_OK;

    /* Free existing R-Tree if any */
    if (ctx->mp_rtree) {
        ct_rtree_free(ctx->mp_rtree);
        ctx->mp_rtree = NULL;
    }

    /* Extract bboxes from multipolygons */
    CTBBox *bboxes = malloc(ctx->num_multipolygons * sizeof(CTBBox));
    if (!bboxes) return CT_ERROR_OUT_OF_MEMORY;

    for (size_t i = 0; i < ctx->num_multipolygons; i++) {
        bboxes[i] = ctx->multipolygons[i].bbox;
    }

    /* Build R-Tree from bboxes */
    ctx->mp_rtree = ct_rtree_build_from_bboxes(bboxes, ctx->num_multipolygons, ctx->bbox);
    free(bboxes);

    if (!ctx->mp_rtree && ctx->num_multipolygons > 0) {
        return CT_ERROR_OUT_OF_MEMORY;
    }

    return CT_OK;
}
