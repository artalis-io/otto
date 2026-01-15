/*
 * FuelWise - Truck Refueling Optimization Library
 * Geospatial Utilities
 *
 * Provides geographic distance calculations and coordinate transformations.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FW_GEO_H
#define FW_GEO_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Distance Calculations
 * ============================================================================ */

/*
 * Calculate the great-circle distance between two points using the Haversine formula.
 *
 * Parameters:
 *   a, b - Geographic coordinates
 *
 * Returns:
 *   Distance in miles
 */
double fw_haversine_distance(FWCoord a, FWCoord b);

/*
 * Calculate the perpendicular distance from a point to a line segment,
 * and find the closest point on the segment.
 *
 * Parameters:
 *   point     - The point to measure from
 *   seg_start - Start of the line segment
 *   seg_end   - End of the line segment
 *   closest   - Output: closest point on segment (can be NULL)
 *
 * Returns:
 *   Perpendicular distance in miles
 */
double fw_point_to_segment_distance(
    FWCoord point,
    FWCoord seg_start,
    FWCoord seg_end,
    FWCoord *closest
);

/* ============================================================================
 * Coordinate Transformations
 * ============================================================================ */

/*
 * Convert lat/lon to local Cartesian coordinates (miles from reference point).
 * Uses equirectangular approximation, suitable for small areas.
 *
 * Parameters:
 *   coord   - Geographic coordinate to convert
 *   ref     - Reference point (origin of local system)
 *   x, y    - Output: local coordinates in miles
 */
void fw_latlon_to_local(FWCoord coord, FWCoord ref, double *x, double *y);

/*
 * Convert local Cartesian coordinates back to lat/lon.
 *
 * Parameters:
 *   x, y    - Local coordinates in miles
 *   ref     - Reference point (origin of local system)
 *   coord   - Output: geographic coordinate
 */
void fw_local_to_latlon(double x, double y, FWCoord ref, FWCoord *coord);

/* ============================================================================
 * Polyline Utilities
 * ============================================================================ */

/*
 * Calculate the total length of a polyline.
 *
 * Parameters:
 *   polyline - The polyline to measure
 *
 * Returns:
 *   Total length in miles
 */
double fw_polyline_length(const FWPolyline *polyline);

/*
 * Find the closest point on a polyline to a given point.
 *
 * Parameters:
 *   point          - The point to measure from
 *   polyline       - The polyline to project onto
 *   segment_index  - Output: index of closest segment (can be NULL)
 *   t              - Output: parameter along segment [0,1] (can be NULL)
 *   closest        - Output: closest point on polyline (can be NULL)
 *
 * Returns:
 *   Distance to closest point in miles, or -1 if polyline is empty
 */
double fw_find_closest_on_polyline(
    FWCoord point,
    const FWPolyline *polyline,
    int *segment_index,
    double *t,
    FWCoord *closest
);

/*
 * Calculate cumulative distance along a polyline to a specific point.
 *
 * Parameters:
 *   polyline      - The polyline
 *   segment_index - Index of the segment containing the point
 *   t             - Parameter along segment [0,1]
 *
 * Returns:
 *   Distance from start of polyline in miles
 */
double fw_distance_along_polyline(
    const FWPolyline *polyline,
    int segment_index,
    double t
);

/*
 * Subsample a polyline to approximately target_points.
 * Useful for performance when working with very detailed polylines.
 *
 * Parameters:
 *   polyline      - The source polyline
 *   target_points - Approximate number of points in output
 *   result        - Output: subsampled polyline (caller must free points)
 *
 * Returns:
 *   0 on success, -1 on error
 */
int fw_subsample_polyline(
    const FWPolyline *polyline,
    int target_points,
    FWPolyline *result
);

#ifdef __cplusplus
}
#endif

#endif /* FW_GEO_H */
