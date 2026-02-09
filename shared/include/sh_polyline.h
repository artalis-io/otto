/*
 * sh_polyline.h - Polyline utilities
 *
 * Provides:
 * - SHPolyline type: array of coordinates with count
 * - Google Polyline encoding/decoding
 * - Polyline geometry: length, projection, distance along
 * - Douglas-Peucker simplification
 *
 * Used by velo (routing), fuelwise (refueling), carta (tiles), clayshards (UI).
 */

#ifndef SH_POLYLINE_H
#define SH_POLYLINE_H

#include "sh_geo.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Polyline Type
 * ============================================================================ */

/*
 * Polyline: array of coordinates with count.
 * Points array is NOT owned by the struct - caller manages memory.
 */
typedef struct {
    SHCoord *points;    /* Array of coordinate points */
    int count;          /* Number of points in the array */
} SHPolyline;

/* ============================================================================
 * Google Polyline Encoding/Decoding
 *
 * Implements the Google Polyline Algorithm for encoding sequences of
 * coordinates into ASCII strings. Used by Google Maps, Mapbox, Valhalla, etc.
 *
 * Reference: https://developers.google.com/maps/documentation/utilities/polylinealgorithm
 * ============================================================================ */

/*
 * Encode a sequence of coordinates to Google Polyline format.
 *
 * @param coords    Array of [lat, lon] pairs (lat0, lon0, lat1, lon1, ...)
 * @param count     Number of coordinate pairs
 * @param precision Decimal precision (5 for standard, 6 for high precision)
 * @param output    Output buffer for encoded string
 * @param capacity  Output buffer capacity
 * @return Number of characters written (excluding null terminator), or 0 on error
 */
size_t sh_polyline_encode(const double *coords, size_t count, int precision,
                          char *output, size_t capacity);

/*
 * Decode a Google Polyline string to coordinates.
 *
 * @param encoded   Null-terminated encoded polyline string
 * @param precision Decimal precision used in encoding
 * @param coords    Output buffer for [lat, lon] pairs
 * @param capacity  Maximum number of coordinate pairs that fit in output
 * @return Number of coordinate pairs decoded, or 0 on error
 */
size_t sh_polyline_decode(const char *encoded, int precision,
                          double *coords, size_t capacity);

/*
 * Calculate maximum buffer size needed for encoding.
 *
 * @param count Number of coordinate pairs
 * @return Maximum bytes needed (including null terminator)
 */
size_t sh_polyline_max_encoded_size(size_t count);

/* ============================================================================
 * Polyline Geometry
 * ============================================================================ */

/*
 * Calculate the total length of a polyline using Haversine distance.
 *
 * @param polyline The polyline to measure
 * @return Total length in meters, or 0 if polyline has < 2 points
 */
double sh_polyline_length(const SHPolyline *polyline);

/*
 * Calculate the perpendicular distance from a point to a line segment,
 * and find the closest point on the segment.
 *
 * @param point     The point to measure from
 * @param seg_start Start of the line segment
 * @param seg_end   End of the line segment
 * @param closest   Output: closest point on segment (can be NULL)
 * @return Perpendicular distance in meters
 */
double sh_point_to_segment_distance(
    SHCoord point,
    SHCoord seg_start,
    SHCoord seg_end,
    SHCoord *closest
);

/*
 * Find the closest point on a polyline to a given point.
 *
 * @param point          The point to measure from
 * @param polyline       The polyline to project onto
 * @param segment_index  Output: index of closest segment (can be NULL)
 * @param t              Output: parameter along segment [0,1] (can be NULL)
 * @param closest        Output: closest point on polyline (can be NULL)
 * @return Distance to closest point in meters, or -1 if polyline is empty
 */
double sh_find_closest_on_polyline(
    SHCoord point,
    const SHPolyline *polyline,
    int *segment_index,
    double *t,
    SHCoord *closest
);

/*
 * Calculate cumulative distance along a polyline to a specific point.
 *
 * @param polyline      The polyline
 * @param segment_index Index of the segment containing the point
 * @param t             Parameter along segment [0,1]
 * @return Distance from start of polyline in meters
 */
double sh_distance_along_polyline(
    const SHPolyline *polyline,
    int segment_index,
    double t
);

/*
 * Subsample a polyline to approximately target_points.
 * Useful for performance when working with very detailed polylines.
 *
 * @param polyline      The source polyline
 * @param target_points Approximate number of points in output
 * @param result        Output: subsampled polyline (caller must free points)
 * @return 0 on success, -1 on error
 */
int sh_subsample_polyline(
    const SHPolyline *polyline,
    int target_points,
    SHPolyline *result
);

/* ============================================================================
 * Douglas-Peucker Simplification
 *
 * Reduces polyline point count while preserving shape.
 * Uses iterative algorithm to avoid stack overflow on large polylines.
 * ============================================================================ */

/* Maximum stack depth for iterative Douglas-Peucker */
#define SH_DP_STACK_SIZE 64

/*
 * Simplify a polyline using Douglas-Peucker algorithm.
 *
 * @param input         Input polyline
 * @param epsilon       Tolerance in degrees (points within epsilon of the
 *                      simplified line are removed)
 * @param output        Output polyline (caller must allocate output->points
 *                      with at least input->count capacity)
 * @param output_capacity Maximum points in output
 * @return Number of points in simplified output, or -1 on error
 */
int sh_polyline_simplify(
    const SHPolyline *input,
    double epsilon,
    SHPolyline *output,
    int output_capacity
);

/*
 * Simplify a polyline with adaptive epsilon.
 * Automatically increases epsilon until output fits within capacity.
 *
 * @param input           Input polyline
 * @param initial_epsilon Starting tolerance in degrees
 * @param output          Output polyline (caller must allocate output->points)
 * @param output_capacity Maximum points in output
 * @return Number of points in simplified output, or -1 on error
 */
int sh_polyline_simplify_adaptive(
    const SHPolyline *input,
    double initial_epsilon,
    SHPolyline *output,
    int output_capacity
);

#ifdef __cplusplus
}
#endif

#endif /* SH_POLYLINE_H */
