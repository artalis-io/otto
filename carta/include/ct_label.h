/*
 * ct_label.h - Map Label Placement
 *
 * Places text labels on map tiles avoiding collisions.
 * Uses the shared font library for text measurement.
 *
 * Usage:
 *   CTLabelPlacer *placer = ct_label_placer_create(256, 256);
 *   ct_label_place_points(placer, ctx, coord, font, 12.0f);
 *   for (int i = 0; i < placer->num_placements; i++) {
 *       CTLabelPlacement *p = &placer->placements[i];
 *       // Draw label at (p->x, p->y)
 *   }
 *   ct_label_placer_free(placer);
 */

#ifndef CT_LABEL_H
#define CT_LABEL_H

#include "ct_types.h"
#include "ct_collision.h"
#include "sh_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/* Label anchor position relative to point */
typedef enum {
    CT_ANCHOR_CENTER = 0,    /* Text centered on point */
    CT_ANCHOR_LEFT,          /* Text to left of point (right-aligned) */
    CT_ANCHOR_RIGHT,         /* Text to right of point (left-aligned) */
    CT_ANCHOR_TOP,           /* Text above point */
    CT_ANCHOR_BOTTOM,        /* Text below point */
    CT_ANCHOR_TOP_LEFT,
    CT_ANCHOR_TOP_RIGHT,
    CT_ANCHOR_BOTTOM_LEFT,
    CT_ANCHOR_BOTTOM_RIGHT,
    CT_ANCHOR_COUNT
} CTLabelAnchor;

/* A placed label */
typedef struct {
    const CTLabeledPoint *point;  /* Source labeled point (owned by PBF context) */
    int x, y;                     /* Text position in tile pixels */
    int width, height;            /* Text bounding box */
    CTLabelAnchor anchor;         /* Anchor used for placement */
    float font_size;              /* Font size used */
} CTLabelPlacement;

/* Label placement context */
typedef struct {
    CTCollisionGrid *collision;   /* Collision detection grid */
    int tile_width;               /* Tile dimensions */
    int tile_height;

    /* Placed labels */
    CTLabelPlacement *placements;
    size_t num_placements;
    size_t placements_capacity;

    /* Configuration */
    int padding_x;                /* Horizontal padding between labels */
    int padding_y;                /* Vertical padding between labels */
    int point_offset;             /* Offset from point to text */
} CTLabelPlacer;

/* Per-glyph placement along a path */
typedef struct {
    float x, y;       /* Glyph center position in tile pixels */
    float angle;       /* Rotation in radians */
} CTPathGlyph;

/* A road label placed along a path */
typedef struct {
    const char *name;
    CTPathGlyph *glyphs;     /* Per-glyph placement (owned) */
    int num_glyphs;
    float font_size;
    int priority;
} CTRoadLabelPlacement;

/* ============================================================================
 * Font Sizing
 * ============================================================================ */

/*
 * Get zoom-adaptive base font size for labels.
 * Returns a base size that varies with zoom level and scales
 * proportionally for larger tile sizes (512px = ~2x of 256px).
 *
 * @param zoom      Zoom level (0-22)
 * @param tile_size Tile size in pixels (256 or 512)
 * @return          Base font size in pixels
 */
float ct_label_base_font_size(int zoom, int tile_size);

/* ============================================================================
 * Placer Lifecycle
 * ============================================================================ */

/*
 * Create a label placer for a tile.
 *
 * @param tile_width   Tile width in pixels (e.g., 256, 512)
 * @param tile_height  Tile height in pixels
 * @return             New placer, or NULL on error
 */
CTLabelPlacer *ct_label_placer_create(int tile_width, int tile_height);

/*
 * Free a label placer.
 */
void ct_label_placer_free(CTLabelPlacer *placer);

/*
 * Reset placer for reuse (clears placements and collision grid).
 */
void ct_label_placer_reset(CTLabelPlacer *placer);

/* ============================================================================
 * Label Placement
 * ============================================================================ */

/*
 * Place point labels from a PBF context onto a tile.
 *
 * Labels are sorted by priority and placed using collision detection.
 * Lower priority labels that would overlap higher priority ones are skipped.
 *
 * @param placer    Label placer
 * @param ctx       PBF context with labeled points
 * @param coord     Tile coordinates (for geographic to pixel conversion)
 * @param font      Font for text measurement
 * @param font_size Base font size in pixels
 * @return          Number of labels placed
 */
int ct_label_place_points(CTLabelPlacer *placer,
                          const CTPBFContext *ctx,
                          CTTileCoord coord,
                          const SHFont *font,
                          float font_size);

/*
 * Try to place a single label.
 *
 * @param placer    Label placer
 * @param point     Labeled point to place
 * @param px, py    Point position in tile pixels
 * @param font      Font for text measurement
 * @param font_size Font size in pixels
 * @return          1 if placed, 0 if skipped due to collision
 */
int ct_label_place_single(CTLabelPlacer *placer,
                          const CTLabeledPoint *point,
                          int px, int py,
                          const SHFont *font,
                          float font_size);

/* ============================================================================
 * Coordinate Conversion
 * ============================================================================ */

/*
 * Convert geographic coordinates to tile pixel coordinates.
 *
 * @param coord     Tile coordinates
 * @param lat, lon  Geographic position
 * @param tile_size Tile size in pixels
 * @param px, py    Output: pixel coordinates (may be outside 0..tile_size-1)
 */
void ct_label_geo_to_pixel(CTTileCoord coord,
                           double lat, double lon,
                           int tile_size,
                           int *px, int *py);

/* ============================================================================
 * Configuration
 * ============================================================================ */

/*
 * Set label padding (space between labels).
 * Default: 4px horizontal, 2px vertical.
 */
void ct_label_set_padding(CTLabelPlacer *placer, int padding_x, int padding_y);

/*
 * Set point offset (distance from point to text).
 * Default: 2px.
 */
void ct_label_set_point_offset(CTLabelPlacer *placer, int offset);

/* ============================================================================
 * Road Label Placement
 * ============================================================================ */

/*
 * Place road labels along named highway ways in a tile.
 * Uses the same collision grid as point labels to prevent overlaps.
 *
 * @param placer      Label placer (shared collision grid with point labels)
 * @param ctx         PBF context with parsed data
 * @param coord       Tile coordinates
 * @param font        Font for text measurement
 * @param tile_size   Tile size in pixels
 * @param out         Output: array of road label placements (caller owns)
 * @param out_count   Output: number of placements
 * @return            Number of road labels placed
 */
int ct_label_place_roads(CTLabelPlacer *placer,
                         const CTPBFContext *ctx, CTTileCoord coord,
                         const SHFont *font, int tile_size,
                         CTRoadLabelPlacement **out, size_t *out_count);

/*
 * Free road label placements.
 */
void ct_label_road_placements_free(CTRoadLabelPlacement *p, size_t count);

/* ============================================================================
 * Area Label Placement
 * ============================================================================ */

/*
 * Place area labels on named polygons (lakes, parks, forests).
 * Uses pole of inaccessibility to find optimal interior point.
 * Labels are horizontal and use the shared collision grid.
 *
 * @param placer      Label placer (shared collision grid)
 * @param ctx         PBF context with parsed data
 * @param coord       Tile coordinates
 * @param font        Font for text measurement
 * @param font_size   Base font size in pixels
 * @return            Number of area labels placed
 */
int ct_label_place_areas(CTLabelPlacer *placer,
                         const CTPBFContext *ctx,
                         CTTileCoord coord,
                         const SHFont *font,
                         float font_size);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/*
 * Get number of labels placed.
 */
int ct_label_get_count(const CTLabelPlacer *placer);

/*
 * Get collision grid occupancy.
 */
float ct_label_get_occupancy(const CTLabelPlacer *placer);

#ifdef __cplusplus
}
#endif

#endif /* CT_LABEL_H */
