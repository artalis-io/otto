/*
 * FuelWise - Truck Refueling Optimization Library
 * Route and Station Filtering
 *
 * Provides functionality for filtering and snapping fuel stations to routes.
 *
 * Copyright (c) 2024. All rights reserved.
 */

#ifndef FW_ROUTE_H
#define FW_ROUTE_H

#include "fw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Station Filtering
 * ============================================================================ */

/*
 * Filter stations by distance to a polyline and snap them to the route.
 *
 * Finds all stations within max_distance (meters) of the route, projects them
 * onto the route, and returns them sorted by distance along the route.
 *
 * Parameters:
 *   stations          - Array of input stations
 *   num_stations      - Number of input stations
 *   polyline          - Route polyline
 *   max_distance      - Maximum perpendicular distance in meters
 *   result            - Output: allocated array of snapped stations
 *   result_count      - Output: number of stations in result
 *
 * Returns:
 *   0 on success, -1 on error
 *
 * Notes:
 *   - Caller must free result->stations using fw_free_snapped_stations()
 *   - Result is sorted by distance_from_start (in meters)
 */
int fw_filter_stations(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *polyline,
    double max_distance,
    FWSnappedStation **result,
    int *result_count
);

/*
 * Filter stations using a two-step approach with deduplication.
 *
 * This is the recommended method for production use. It:
 * 1. Pre-filters stations using the overview polyline for speed
 * 2. Snaps stations to a subsampled version of the detailed polyline
 * 3. Handles route loop-backs where stations may appear multiple times
 * 4. Applies deduplication strategy for repeated stations
 *
 * Parameters:
 *   stations          - Array of input stations
 *   num_stations      - Number of input stations
 *   detailed          - Detailed route polyline (high resolution)
 *   overview          - Overview route polyline (optional, can be NULL)
 *   config            - Filtering configuration
 *   result            - Output: allocated array of snapped stations
 *   result_count      - Output: number of stations in result
 *
 * Returns:
 *   0 on success, -1 on error
 *
 * Notes:
 *   - If overview is NULL, only the detailed polyline is used
 *   - Caller must free result using fw_free_snapped_stations()
 *   - Result is sorted by distance_from_start
 */
int fw_filter_stations_two_step(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *detailed,
    const FWPolyline *overview,
    const FWFilterConfig *config,
    FWSnappedStation **result,
    int *result_count
);

/*
 * Free an array of snapped stations.
 *
 * Parameters:
 *   stations - Array to free (can be NULL)
 */
void fw_free_snapped_stations(FWSnappedStation *stations);

/*
 * FW_FUTURE_API: Free a filter result structure.
 *
 * Parameters:
 *   result - Result to free (can be NULL)
 */
FW_FUTURE_API void fw_free_filter_result(FWFilterResult *result);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * FW_FUTURE_API: Sort snapped stations by distance from start.
 *
 * Parameters:
 *   stations - Array of snapped stations
 *   count    - Number of stations
 */
FW_FUTURE_API void fw_sort_stations_by_distance(FWSnappedStation *stations, int count);

/*
 * FW_FUTURE_API: Remove duplicate stations from a sorted array.
 *
 * Parameters:
 *   stations - Array of snapped stations (must be sorted)
 *   count    - Input/Output: number of stations
 *   strategy - Deduplication strategy
 *   max_dedup_distance - Max distance to consider stations as duplicates
 */
FW_FUTURE_API void fw_deduplicate_stations(
    FWSnappedStation *stations,
    int *count,
    FWDedupStrategy strategy,
    double max_dedup_distance
);

#ifdef __cplusplus
}
#endif

#endif /* FW_ROUTE_H */
