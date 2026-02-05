/*
 * ct_simplify.h - Geometry simplification for map tiles
 *
 * Provides in-place Douglas-Peucker simplification to reduce point counts
 * at lower zoom levels while preserving visual shape.
 *
 * Note: These functions modify arrays in place. For non-destructive
 * simplification, use ct_simplify_linestring() in ct_tile.h.
 */

#ifndef CT_SIMPLIFY_H
#define CT_SIMPLIFY_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * In-Place Simplification Functions
 * ============================================================================ */

/*
 * Simplify a linestring in place using Douglas-Peucker algorithm.
 *
 * Points are modified in place. The first and last points are always kept.
 *
 * @param points     Array of tile points (modified in place)
 * @param num_points Input: original count, Output: simplified count
 * @param tolerance  Maximum perpendicular distance (in tile units)
 */
void ct_simplify_line_inplace(CTTilePoint *points, int *num_points, float tolerance);

/*
 * Simplify a polygon in place using Douglas-Peucker algorithm.
 *
 * The polygon is treated as a closed ring. At least 3 points are always kept.
 *
 * @param points     Array of tile points (modified in place)
 * @param num_points Input: original count, Output: simplified count
 * @param tolerance  Maximum perpendicular distance (in tile units)
 */
void ct_simplify_poly_inplace(CTTilePoint *points, int *num_points, float tolerance);

/*
 * Simplify a multipolygon (polygon with holes) in place.
 *
 * Each ring is simplified separately to preserve the ring structure.
 * The ring_ends array is updated to reflect new point counts.
 *
 * @param points     Array of tile points (modified in place)
 * @param num_points Input: original count, Output: simplified count
 * @param ring_ends  Array of ring end indices (modified in place)
 * @param num_rings  Number of rings (unchanged)
 * @param tolerance  Maximum perpendicular distance (in tile units)
 */
void ct_simplify_multipolygon_inplace(CTTilePoint *points, int *num_points,
                                      int *ring_ends, int num_rings,
                                      float tolerance);

/*
 * Simplify geographic coordinates in place using Douglas-Peucker algorithm.
 *
 * Coordinates are modified in place. Works with lat/lon directly.
 *
 * @param coords     Array of coordinates (modified in place)
 * @param num_coords Input: original count, Output: simplified count
 * @param tolerance  Maximum perpendicular distance (in degrees)
 */
void ct_simplify_coords_inplace(CTCoord *coords, int *num_coords, double tolerance);

/* ============================================================================
 * Tolerance Calculation
 * ============================================================================ */

/*
 * Get appropriate simplification tolerance for a zoom level.
 *
 * Tolerance is calibrated to be approximately 1 pixel at the given zoom.
 * Lower zoom = more aggressive simplification.
 *
 * @param zoom Zoom level (0-22)
 * @return Tolerance in tile coordinate units (extent = 4096)
 */
float ct_simplify_tolerance(int zoom);

/*
 * Get geographic simplification tolerance for a zoom level.
 *
 * @param zoom Zoom level (0-22)
 * @return Tolerance in degrees
 */
double ct_simplify_tolerance_degrees(int zoom);

#ifdef __cplusplus
}
#endif

#endif /* CT_SIMPLIFY_H */
