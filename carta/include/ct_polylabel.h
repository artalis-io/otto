/*
 * ct_polylabel.h - Pole of Inaccessibility for Polygon Labeling
 *
 * Finds the internal point with maximum distance to any polygon edge,
 * optimal for placing area labels (lakes, parks, forests).
 *
 * Based on the Mapbox polylabel algorithm (ISC license):
 * https://github.com/mapbox/polylabel
 *
 * Uses iterative cell subdivision with a priority queue (sh_heap)
 * to find the optimal point efficiently.
 */

#ifndef CT_POLYLABEL_H
#define CT_POLYLABEL_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Find the pole of inaccessibility for a simple polygon.
 *
 * The pole of inaccessibility is the internal point with maximum
 * distance to any polygon edge. This is the optimal position for
 * placing a label inside the polygon.
 *
 * @param coords     Polygon coordinates (closed ring)
 * @param num_coords Number of coordinates
 * @param precision  Desired precision in coordinate units (e.g., 0.001)
 * @param out_x      Output: X coordinate of pole
 * @param out_y      Output: Y coordinate of pole
 * @param out_dist   Output: distance from pole to nearest edge
 * @return           1 on success, 0 on error
 */
int ct_polylabel(const CTCoord *coords, int num_coords,
                 double precision,
                 double *out_x, double *out_y, double *out_dist);

/*
 * Find the pole of inaccessibility for a polygon with holes.
 *
 * @param rings       Array of ring coordinate arrays
 * @param ring_sizes  Array of ring sizes
 * @param num_rings   Number of rings (first is outer, rest are holes)
 * @param precision   Desired precision in coordinate units
 * @param out_x       Output: X coordinate of pole
 * @param out_y       Output: Y coordinate of pole
 * @param out_dist    Output: distance from pole to nearest edge
 * @return            1 on success, 0 on error
 */
int ct_polylabel_with_holes(const CTCoord **rings, const int *ring_sizes,
                             int num_rings, double precision,
                             double *out_x, double *out_y, double *out_dist);

#ifdef __cplusplus
}
#endif

#endif /* CT_POLYLABEL_H */
