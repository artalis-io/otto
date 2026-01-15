/*
 * FuelWise - Truck Refueling Optimization Library
 * Route and Station Filtering Implementation
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include "fw_route.h"
#include "fw_geo.h"

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/*
 * Comparison function for sorting snapped stations by distance from start.
 */
static int compare_snapped_stations(const void *a, const void *b)
{
    const FWSnappedStation *sa = (const FWSnappedStation *)a;
    const FWSnappedStation *sb = (const FWSnappedStation *)b;

    if (sa->distance_from_start < sb->distance_from_start) return -1;
    if (sa->distance_from_start > sb->distance_from_start) return 1;
    return 0;
}

/*
 * Check if a station is within max_radius of any segment in a polyline.
 * Returns the minimum distance if within radius, or -1 if outside.
 * Optimized for filtering: returns early once a match is found.
 */
static double station_min_distance_to_polyline(
    FWCoord station,
    const FWPolyline *polyline,
    double max_radius_miles)
{
    if (polyline == NULL || polyline->num_points < 2) {
        return -1.0;
    }

    double min_dist = max_radius_miles + 1.0;

    for (int i = 0; i < polyline->num_points - 1; i++) {
        double dist = fw_point_to_segment_distance(
            station,
            polyline->points[i],
            polyline->points[i + 1],
            NULL
        );

        if (dist < min_dist) {
            min_dist = dist;
            /* Early exit if we found a segment within radius */
            if (min_dist <= max_radius_miles) {
                return min_dist;
            }
        }
    }

    return (min_dist <= max_radius_miles) ? min_dist : -1.0;
}

/*
 * Create a boolean mask of which stations are within radius of a polyline.
 * Returns an array of num_stations booleans (caller must free).
 */
static int* create_station_filter_mask(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *polyline,
    double max_radius_miles)
{
    int *mask = calloc(num_stations, sizeof(int));
    if (!mask) return NULL;

    for (int i = 0; i < num_stations; i++) {
        double dist = station_min_distance_to_polyline(
            stations[i].location,
            polyline,
            max_radius_miles
        );
        mask[i] = (dist >= 0.0) ? 1 : 0;
    }

    return mask;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int fw_filter_stations(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *polyline,
    double max_distance_miles,
    FWSnappedStation **result,
    int *result_count)
{
    *result = NULL;
    *result_count = 0;

    if (stations == NULL || num_stations == 0 ||
        polyline == NULL || polyline->num_points < 2) {
        return 0;  /* Empty result is not an error */
    }

    /* Allocate temporary array for all stations */
    FWSnappedStation *temp = malloc(num_stations * sizeof(FWSnappedStation));
    if (!temp) return -1;

    int count = 0;

    for (int i = 0; i < num_stations; i++) {
        int seg_idx;
        double t;
        FWCoord closest;

        double perp_dist = fw_find_closest_on_polyline(
            stations[i].location,
            polyline,
            &seg_idx,
            &t,
            &closest
        );

        if (perp_dist >= 0.0 && perp_dist <= max_distance_miles) {
            temp[count].station_id = stations[i].id;
            temp[count].distance_from_start = fw_distance_along_polyline(polyline, seg_idx, t);
            temp[count].perpendicular_distance = perp_dist;
            temp[count].snap_point = closest;
            temp[count].price_per_gallon = stations[i].price_per_gallon;
            count++;
        }
    }

    if (count == 0) {
        free(temp);
        return 0;
    }

    /* Sort by distance from start */
    qsort(temp, count, sizeof(FWSnappedStation), compare_snapped_stations);

    /* Shrink allocation to actual size */
    FWSnappedStation *final = realloc(temp, count * sizeof(FWSnappedStation));
    *result = final ? final : temp;
    *result_count = count;

    return 0;
}

int fw_filter_stations_two_step(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *detailed,
    const FWPolyline *overview,
    const FWFilterConfig *config,
    FWSnappedStation **result,
    int *result_count)
{
    *result = NULL;
    *result_count = 0;

    (void)overview;  /* Not used in current implementation */

    if (stations == NULL || num_stations == 0 ||
        detailed == NULL || detailed->num_points < 2 ||
        config == NULL) {
        return 0;
    }

    /* Step 1: Create filter mask using detailed polyline */
    int *filter_mask = create_station_filter_mask(
        stations, num_stations,
        detailed,
        config->max_distance_miles
    );

    if (!filter_mask) {
        return -1;
    }

    /* Count filtered stations and create filtered array */
    int filtered_count = 0;
    for (int i = 0; i < num_stations; i++) {
        if (filter_mask[i]) filtered_count++;
    }

    if (filtered_count == 0) {
        free(filter_mask);
        return 0;
    }

    /* Create array of filtered stations */
    FWStation *filtered_stations = malloc(filtered_count * sizeof(FWStation));
    if (!filtered_stations) {
        free(filter_mask);
        return -1;
    }

    int idx = 0;
    for (int i = 0; i < num_stations; i++) {
        if (filter_mask[i]) {
            filtered_stations[idx++] = stations[i];
        }
    }
    free(filter_mask);

    /* Step 2: Subsample detailed polyline (target ~2000 points for performance) */
    FWPolyline subsampled = {NULL, 0};
    int target_points = 2000;

    if (fw_subsample_polyline(detailed, target_points, &subsampled) != 0) {
        free(filtered_stations);
        return -1;
    }

    /* Step 3: Walk subsampled polyline and record stations as we encounter them */
    /* Track last occurrence distance for each filtered station */
    double *last_along_dist = calloc(filtered_count, sizeof(double));
    if (!last_along_dist) {
        free(subsampled.points);
        free(filtered_stations);
        return -1;
    }

    /* Collect results - allow multiple occurrences */
    int capacity = filtered_count * 4;
    FWSnappedStation *collected = malloc(capacity * sizeof(FWSnappedStation));
    if (!collected) {
        free(last_along_dist);
        free(subsampled.points);
        free(filtered_stations);
        return -1;
    }
    int collected_count = 0;

    double along_polyline = 0.0;

    for (int seg = 0; seg < subsampled.num_points - 1; seg++) {
        FWCoord seg_start = subsampled.points[seg];
        FWCoord seg_end = subsampled.points[seg + 1];

        double seg_length = fw_haversine_distance(seg_start, seg_end);

        /* Check each filtered station */
        for (int st = 0; st < filtered_count; st++) {
            FWCoord snap;
            double perp_dist = fw_point_to_segment_distance(
                filtered_stations[st].location,
                seg_start,
                seg_end,
                &snap
            );

            if (perp_dist <= config->max_distance_miles) {
                double along_segment = fw_haversine_distance(seg_start, snap);
                double current_along = along_polyline + along_segment;

                /* Check min distance between identical points */
                double last_dist = last_along_dist[st];
                if (last_dist == 0.0 ||
                    (current_along - last_dist) >= config->min_repeat_distance_miles) {

                    last_along_dist[st] = current_along;

                    /* Grow array if needed */
                    if (collected_count >= capacity) {
                        capacity *= 2;
                        FWSnappedStation *new_collected = realloc(collected,
                            capacity * sizeof(FWSnappedStation));
                        if (!new_collected) break;
                        collected = new_collected;
                    }

                    collected[collected_count].station_id = filtered_stations[st].id;
                    collected[collected_count].distance_from_start = current_along;
                    collected[collected_count].price_per_gallon = filtered_stations[st].price_per_gallon;
                    collected[collected_count].perpendicular_distance = perp_dist;
                    collected[collected_count].snap_point = snap;
                    collected_count++;
                }
            }
        }

        along_polyline += seg_length;
    }

    free(last_along_dist);
    free(subsampled.points);
    free(filtered_stations);

    if (collected_count == 0) {
        free(collected);
        return 0;
    }

    /* Sort by distance from start */
    qsort(collected, collected_count, sizeof(FWSnappedStation), compare_snapped_stations);

    /* Apply sequential deduplication if requested */
    if (config->dedup_strategy == FW_DEDUP_NONE) {
        FWSnappedStation *shrunk = realloc(collected, collected_count * sizeof(FWSnappedStation));
        *result = shrunk ? shrunk : collected;
        *result_count = collected_count;
        return 0;
    }

    /* Deduplicate consecutive same-ID entries within max_dedup_distance */
    FWSnappedStation *deduped = malloc(collected_count * sizeof(FWSnappedStation));
    if (!deduped) {
        *result = collected;
        *result_count = collected_count;
        return 0;
    }

    int dedup_count = 0;
    deduped[dedup_count++] = collected[0];

    for (int i = 1; i < collected_count; i++) {
        FWSnappedStation *curr = &collected[i];
        FWSnappedStation *last = &deduped[dedup_count - 1];

        int can_dedup = 0;
        if (curr->station_id == last->station_id) {
            double dist_between = curr->distance_from_start - last->distance_from_start;
            if (dist_between < config->max_dedup_distance_miles) {
                can_dedup = 1;
            }
        }

        if (can_dedup) {
            switch (config->dedup_strategy) {
                case FW_DEDUP_FIRST:
                    /* Keep first, do nothing */
                    break;
                case FW_DEDUP_LAST:
                    *last = *curr;
                    break;
                case FW_DEDUP_CLOSEST:
                    if (curr->perpendicular_distance < last->perpendicular_distance) {
                        *last = *curr;
                    }
                    break;
                default:
                    break;
            }
        } else {
            deduped[dedup_count++] = *curr;
        }
    }

    free(collected);

    FWSnappedStation *shrunk = realloc(deduped, dedup_count * sizeof(FWSnappedStation));
    *result = shrunk ? shrunk : deduped;
    *result_count = dedup_count;

    return 0;
}

void fw_free_snapped_stations(FWSnappedStation *stations)
{
    free(stations);
}

void fw_free_filter_result(FWFilterResult *result)
{
    if (result) {
        free(result->stations);
        result->stations = NULL;
        result->count = 0;
    }
}

void fw_sort_stations_by_distance(FWSnappedStation *stations, int count)
{
    if (stations && count > 1) {
        qsort(stations, count, sizeof(FWSnappedStation), compare_snapped_stations);
    }
}

void fw_deduplicate_stations(
    FWSnappedStation *stations,
    int *count,
    FWDedupStrategy strategy,
    double max_dedup_distance)
{
    if (!stations || !count || *count < 2 || strategy == FW_DEDUP_NONE) {
        return;
    }

    int write_idx = 0;

    for (int read_idx = 1; read_idx < *count; read_idx++) {
        FWSnappedStation *curr = &stations[read_idx];
        FWSnappedStation *last = &stations[write_idx];

        int can_dedup = 0;
        if (curr->station_id == last->station_id) {
            double dist_between = curr->distance_from_start - last->distance_from_start;
            if (dist_between < max_dedup_distance) {
                can_dedup = 1;
            }
        }

        if (can_dedup) {
            switch (strategy) {
                case FW_DEDUP_FIRST:
                    break;
                case FW_DEDUP_LAST:
                    *last = *curr;
                    break;
                case FW_DEDUP_CLOSEST:
                    if (curr->perpendicular_distance < last->perpendicular_distance) {
                        *last = *curr;
                    }
                    break;
                default:
                    break;
            }
        } else {
            stations[++write_idx] = *curr;
        }
    }

    *count = write_idx + 1;
}
