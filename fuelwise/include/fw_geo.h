/*
 * FuelWise - Truck Refueling Optimization Library
 * Geospatial Utilities
 *
 * NOTE: Most polyline functions have been moved to shared library (sh_polyline.h).
 * This header is kept for backward compatibility but most functions are now
 * simple aliases to shared library functions.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FW_GEO_H
#define FW_GEO_H

#include "fw_types.h"
#include "sh_polyline.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Distance Calculations
 *
 * Use sh_haversine() directly for Haversine distance.
 * Use sh_point_to_segment_distance() directly for segment distance.
 * ============================================================================ */

/* Deprecated: Use sh_haversine() directly */
#define fw_haversine_distance(a, b) sh_haversine((a), (b))

/* Deprecated: Use sh_point_to_segment_distance() directly */
#define fw_point_to_segment_distance(point, seg_start, seg_end, closest) \
    sh_point_to_segment_distance((point), (seg_start), (seg_end), (closest))

/* ============================================================================
 * Coordinate Transformations
 *
 * Use sh_latlon_to_local() and sh_local_to_latlon() directly.
 * ============================================================================ */

/* Deprecated: Use sh_latlon_to_local() directly */
#define fw_latlon_to_local(coord, ref, x, y) sh_latlon_to_local((coord), (ref), (x), (y))

/* Deprecated: Use sh_local_to_latlon() directly */
#define fw_local_to_latlon(x, y, ref, coord) sh_local_to_latlon((x), (y), (ref), (coord))

/* ============================================================================
 * Polyline Utilities
 *
 * Use sh_polyline_* functions directly. Note: field name changed from
 * 'num_points' to 'count' in SHPolyline.
 * ============================================================================ */

/* Deprecated: Use sh_polyline_length() directly */
#define fw_polyline_length(polyline) sh_polyline_length((polyline))

/* Deprecated: Use sh_find_closest_on_polyline() directly */
#define fw_find_closest_on_polyline(point, polyline, seg_idx, t, closest) \
    sh_find_closest_on_polyline((point), (polyline), (seg_idx), (t), (closest))

/* Deprecated: Use sh_distance_along_polyline() directly */
#define fw_distance_along_polyline(polyline, seg_idx, t) \
    sh_distance_along_polyline((polyline), (seg_idx), (t))

/* Deprecated: Use sh_subsample_polyline() directly */
#define fw_subsample_polyline(polyline, target, result) \
    sh_subsample_polyline((polyline), (target), (result))

#ifdef __cplusplus
}
#endif

#endif /* FW_GEO_H */
