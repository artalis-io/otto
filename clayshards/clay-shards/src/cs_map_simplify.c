/**
 * Clay Components - Polyline Simplification
 *
 * Douglas-Peucker algorithm for reducing polyline point count.
 *
 * NOTE: Implementation moved to shared library (sh_polyline.c).
 * This file provides a wrapper that adapts CsGeoPoint to SHCoord.
 */

#include "cs_map_internal.h"
#include "sh_polyline.h"
#include <string.h>  /* For memcpy */

/* ============================================================================
 * Public Simplification API
 *
 * CsGeoPoint and SHCoord have the same layout (double lat, double lon),
 * so we can cast between them safely.
 * ============================================================================ */

/**
 * Simplify a polyline using Douglas-Peucker algorithm with adaptive epsilon.
 * Automatically increases epsilon until output fits within capacity.
 * Returns the number of points in the simplified output.
 *
 * @param points      Input points
 * @param count       Number of input points
 * @param epsilon     Initial tolerance in degrees
 * @param out         Output buffer (can be same as input for in-place)
 * @param out_capacity Maximum output points
 */
int cs_map_simplify_polyline(
    const CsGeoPoint *points,
    int count,
    double epsilon,
    CsGeoPoint *out,
    int out_capacity
) {
    if (count <= 2) {
        /* Nothing to simplify */
        int n = (count < out_capacity) ? count : out_capacity;
        if (out != points) {
            memcpy(out, points, n * sizeof(CsGeoPoint));
        }
        return n;
    }

    /*
     * CsGeoPoint and SHCoord have identical layout:
     *   struct { double lat; double lon; }
     * So we can safely cast between them.
     */
    SHPolyline input = {
        .points = (SHCoord *)points,
        .count = count
    };

    SHPolyline output = {
        .points = (SHCoord *)out,
        .count = 0
    };

    /* Use adaptive simplification to ensure output fits within capacity */
    int result = sh_polyline_simplify_adaptive(&input, epsilon, &output, out_capacity);

    return (result > 0) ? result : 0;
}
