/*
 * FuelWise - Truck Refueling Optimization Library
 * Geospatial Utilities Implementation
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include "fw_geo.h"
#include "sh_geo.h"
#include "sh_units.h"

/* ============================================================================
 * Distance Calculations
 *
 * FuelWise uses SI units internally:
 * - Distance: meters
 * - Volume: liters
 * - Fuel efficiency: L/100km
 * - Mass: kilograms
 *
 * Unit conversions happen at API boundaries, not here.
 * ============================================================================ */

double fw_haversine_distance(FWCoord a, FWCoord b)
{
    /* sh_haversine returns meters, which we use internally */
    return sh_haversine(a, b);
}

/*
 * Project a point onto a line segment in local Cartesian coordinates.
 * Returns the parameter t along the segment [0,1].
 */
static double project_point_on_segment(
    double px, double py,
    double ax, double ay,
    double bx, double by,
    double *proj_x, double *proj_y)
{
    double dx = bx - ax;
    double dy = by - ay;
    double len_sq = dx * dx + dy * dy;

    if (len_sq < 1e-12) {
        /* Degenerate segment */
        *proj_x = ax;
        *proj_y = ay;
        return 0.0;
    }

    double t = ((px - ax) * dx + (py - ay) * dy) / len_sq;

    /* Clamp t to [0, 1] */
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    *proj_x = ax + t * dx;
    *proj_y = ay + t * dy;
    return t;
}

double fw_point_to_segment_distance(
    FWCoord point,
    FWCoord seg_start,
    FWCoord seg_end,
    FWCoord *closest)
{
    /* Use midpoint as reference for local coordinate system */
    FWCoord ref = {
        .lat = (seg_start.lat + seg_end.lat) / 2.0,
        .lon = (seg_start.lon + seg_end.lon) / 2.0
    };

    /* Convert all points to local Cartesian */
    double px, py, ax, ay, bx, by;
    fw_latlon_to_local(point, ref, &px, &py);
    fw_latlon_to_local(seg_start, ref, &ax, &ay);
    fw_latlon_to_local(seg_end, ref, &bx, &by);

    /* Project point onto segment */
    double proj_x, proj_y;
    project_point_on_segment(px, py, ax, ay, bx, by, &proj_x, &proj_y);

    /* Convert projection back to lat/lon if requested */
    if (closest != NULL) {
        fw_local_to_latlon(proj_x, proj_y, ref, closest);
    }

    /* Calculate distance in local coordinates */
    double dx = px - proj_x;
    double dy = py - proj_y;
    return sqrt(dx * dx + dy * dy);
}

/* ============================================================================
 * Coordinate Transformations
 * ============================================================================ */

void fw_latlon_to_local(FWCoord coord, FWCoord ref, double *x, double *y)
{
    double cos_lat = cos(ref.lat * FW_DEG_TO_RAD);
    *x = (coord.lon - ref.lon) * FW_DEG_TO_RAD * SH_EARTH_RADIUS_M * cos_lat;
    *y = (coord.lat - ref.lat) * FW_DEG_TO_RAD * SH_EARTH_RADIUS_M;
}

void fw_local_to_latlon(double x, double y, FWCoord ref, FWCoord *coord)
{
    double cos_lat = cos(ref.lat * FW_DEG_TO_RAD);

    /* Guard against division by near-zero at poles (|lat| > 89.9 degrees).
     * Trucking routes don't go to poles, but handle gracefully. */
    if (cos_lat < 1e-6) {
        cos_lat = 1e-6;
    }

    coord->lon = ref.lon + (x / (SH_EARTH_RADIUS_M * cos_lat)) * FW_RAD_TO_DEG;
    coord->lat = ref.lat + (y / SH_EARTH_RADIUS_M) * FW_RAD_TO_DEG;
}

/* ============================================================================
 * Polyline Utilities
 * ============================================================================ */

double fw_polyline_length(const FWPolyline *polyline)
{
    if (polyline == NULL || polyline->num_points < 2) {
        return 0.0;
    }

    double total = 0.0;
    for (int i = 0; i < polyline->num_points - 1; i++) {
        total += fw_haversine_distance(polyline->points[i], polyline->points[i + 1]);
    }
    return total;
}

double fw_find_closest_on_polyline(
    FWCoord point,
    const FWPolyline *polyline,
    int *segment_index,
    double *t,
    FWCoord *closest)
{
    if (polyline == NULL || polyline->num_points < 1) {
        return -1.0;
    }

    if (polyline->num_points == 1) {
        if (segment_index) *segment_index = 0;
        if (t) *t = 0.0;
        if (closest) *closest = polyline->points[0];
        return fw_haversine_distance(point, polyline->points[0]);
    }

    double min_distance = 1e18;
    int best_segment = 0;
    double best_t = 0.0;
    FWCoord best_closest = polyline->points[0];

    for (int i = 0; i < polyline->num_points - 1; i++) {
        FWCoord snap;
        double dist = fw_point_to_segment_distance(
            point,
            polyline->points[i],
            polyline->points[i + 1],
            &snap
        );

        if (dist < min_distance) {
            min_distance = dist;
            best_segment = i;
            best_closest = snap;

            /* Calculate t parameter */
            double seg_len = fw_haversine_distance(
                polyline->points[i],
                polyline->points[i + 1]
            );
            double dist_to_snap = fw_haversine_distance(
                polyline->points[i],
                snap
            );
            best_t = (seg_len > 1e-9) ? dist_to_snap / seg_len : 0.0;
        }
    }

    if (segment_index) *segment_index = best_segment;
    if (t) *t = best_t;
    if (closest) *closest = best_closest;
    return min_distance;
}

double fw_distance_along_polyline(
    const FWPolyline *polyline,
    int segment_index,
    double t)
{
    if (polyline == NULL || polyline->num_points < 2) {
        return 0.0;
    }

    /* Bounds check on segment_index */
    if (segment_index < 0 || segment_index >= polyline->num_points) {
        return 0.0;
    }

    double total_distance = 0.0;

    /* Sum up complete segments before the target segment */
    for (int i = 0; i < segment_index && i < polyline->num_points - 1; i++) {
        total_distance += fw_haversine_distance(
            polyline->points[i],
            polyline->points[i + 1]
        );
    }

    /* Add partial distance within the target segment */
    if (segment_index < polyline->num_points - 1) {
        double seg_len = fw_haversine_distance(
            polyline->points[segment_index],
            polyline->points[segment_index + 1]
        );
        total_distance += t * seg_len;
    }

    return total_distance;
}

int fw_subsample_polyline(
    const FWPolyline *polyline,
    int target_points,
    FWPolyline *result)
{
    if (polyline == NULL || result == NULL || target_points < 2) {
        return -1;
    }

    if (polyline->num_points <= target_points) {
        /* No subsampling needed, copy the polyline */
        result->num_points = polyline->num_points;
        /* Check for overflow before allocation */
        if (result->num_points < 0 ||
            (size_t)result->num_points > SIZE_MAX / sizeof(FWCoord)) {
            return -1;
        }
        result->points = malloc((size_t)result->num_points * sizeof(FWCoord));
        if (result->points == NULL) {
            return -1;
        }
        for (int i = 0; i < result->num_points; i++) {
            result->points[i] = polyline->points[i];
        }
        return 0;
    }

    /* Calculate step size */
    int step = (polyline->num_points - 1) / (target_points - 1);
    if (step < 1) step = 1;

    /* Count actual points we'll include */
    int count = 0;
    for (int i = 0; i < polyline->num_points; i += step) {
        count++;
    }
    /* Always include last point */
    if ((polyline->num_points - 1) % step != 0) {
        count++;
    }

    /* Allocate result - check for overflow */
    if (count < 0 || (size_t)count > SIZE_MAX / sizeof(FWCoord)) {
        return -1;
    }
    result->points = malloc((size_t)count * sizeof(FWCoord));
    if (result->points == NULL) {
        return -1;
    }
    result->num_points = count;

    /* Copy points */
    int idx = 0;
    for (int i = 0; i < polyline->num_points && idx < count - 1; i += step) {
        result->points[idx++] = polyline->points[i];
    }
    /* Always include last point */
    result->points[idx] = polyline->points[polyline->num_points - 1];

    return 0;
}
