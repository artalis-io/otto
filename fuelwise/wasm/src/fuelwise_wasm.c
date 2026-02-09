/*
 * FuelWise WebAssembly Bindings
 *
 * Entry points for the WASM module, designed for easy JavaScript interop.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "fuelwise.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

/* ============================================================================
 * Memory Management - for JavaScript interop
 * ============================================================================ */

WASM_EXPORT
void* wasm_malloc(int size) {
    return malloc(size);
}

WASM_EXPORT
void wasm_free(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * Version
 * ============================================================================ */

WASM_EXPORT
const char* wasm_version(void) {
    return fw_version();
}

/* ============================================================================
 * Geospatial Functions
 * ============================================================================ */

WASM_EXPORT
double wasm_haversine(double lat1, double lon1, double lat2, double lon2) {
    FWCoord a = {lat1, lon1};
    FWCoord b = {lat2, lon2};
    return fw_haversine_distance(a, b);
}

WASM_EXPORT
double wasm_polyline_length(double *points, int num_points) {
    /* Validate inputs */
    if (!points || num_points < 2) return 0.0;

    FWPolyline poly;
    poly.points = (FWCoord*)points;
    poly.count = num_points;
    return fw_polyline_length(&poly);
}

/* ============================================================================
 * Station Filtering
 *
 * Input arrays are flat: [lat0, lon0, price0, id0, lat1, lon1, price1, id1, ...]
 * Route is flat: [lat0, lon0, lat1, lon1, ...]
 * Output is written to provided buffer.
 * ============================================================================ */

WASM_EXPORT
int wasm_filter_stations(
    double *stations_flat,      /* [lat, lon, price, id] x num_stations */
    int num_stations,
    double *route_flat,         /* [lat, lon] x num_points */
    int num_points,
    double max_distance,
    double *result_buffer,      /* Output: [id, dist_from_start, perp_dist, price] x filtered */
    int result_buffer_size)
{
    /* Validate inputs */
    if (!stations_flat || num_stations <= 0 ||
        !route_flat || num_points < 2 ||
        !result_buffer || result_buffer_size <= 0) {
        return -1;
    }

    /* Build station array */
    FWStation *stations = malloc((size_t)num_stations * sizeof(FWStation));
    if (!stations) return -1;

    for (int i = 0; i < num_stations; i++) {
        stations[i].location.lat = stations_flat[i * 4 + 0];
        stations[i].location.lon = stations_flat[i * 4 + 1];
        stations[i].price_per_gallon = stations_flat[i * 4 + 2];
        stations[i].id = (int)stations_flat[i * 4 + 3];
        stations[i].name = NULL;
    }

    /* Build polyline */
    FWPolyline route;
    route.points = (FWCoord*)route_flat;
    route.count = num_points;

    /* Filter */
    FWSnappedStation *filtered = NULL;
    int filtered_count = 0;
    int ret = fw_filter_stations(stations, num_stations, &route, max_distance,
                                  &filtered, &filtered_count);
    free(stations);

    if (ret != 0) return -1;

    /* Copy to result buffer */
    int max_results = result_buffer_size / 4;
    int copy_count = (filtered_count < max_results) ? filtered_count : max_results;

    for (int i = 0; i < copy_count; i++) {
        result_buffer[i * 4 + 0] = filtered[i].station_id;
        result_buffer[i * 4 + 1] = filtered[i].distance_from_start;
        result_buffer[i * 4 + 2] = filtered[i].perpendicular_distance;
        result_buffer[i * 4 + 3] = filtered[i].price_per_gallon;
    }

    fw_free_snapped_stations(filtered);
    return copy_count;
}

/* ============================================================================
 * Refueling Optimization - Simple API
 *
 * Stations are already snapped: [id, distance_from_start, price] x num_stations
 * Result: [gallons_at_station_0, gallons_at_station_1, ...] + metadata
 * ============================================================================ */

WASM_EXPORT
int wasm_solve_simple(
    double *stations_flat,      /* [id, distance, price] x num_stations */
    int num_stations,
    double total_distance,
    double tank_capacity,
    double current_fuel,
    double consumption_mpg,
    double minimum_fuel,
    double *result_purchases,   /* Output: gallons at each station */
    double *result_meta)        /* Output: [status, num_stops, total_cost, remaining_fuel] */
{
    /* Validate inputs */
    if (!stations_flat || num_stations <= 0 ||
        !result_purchases || !result_meta) {
        return -1;
    }

    /* Build problem */
    FWRefuelProblem problem;
    memset(&problem, 0, sizeof(problem));

    problem.total_distance = total_distance;
    problem.tank_capacity = tank_capacity;
    problem.current_fuel = current_fuel;
    problem.base_consumption_mpg = consumption_mpg;
    problem.minimum_fuel = minimum_fuel;
    problem.minimum_fuel_at_end = minimum_fuel;
    problem.num_stations = num_stations;

    /* Build stations */
    FWSnappedStation *stations = malloc((size_t)num_stations * sizeof(FWSnappedStation));
    if (!stations) return -1;

    for (int i = 0; i < num_stations; i++) {
        stations[i].station_id = (int)stations_flat[i * 3 + 0];
        stations[i].distance_from_start = stations_flat[i * 3 + 1];
        stations[i].price_per_gallon = stations_flat[i * 3 + 2];
        stations[i].perpendicular_distance = 0;
    }
    problem.stations = stations;

    /* Solve */
    FWRefuelSolution solution;
    int ret = fw_solve_refuel_lp(&problem, &solution);

    if (ret == 0 && solution.status == FW_STATUS_OPTIMAL) {
        for (int i = 0; i < num_stations; i++) {
            result_purchases[i] = solution.purchases[i];
        }
        result_meta[0] = solution.status;
        result_meta[1] = solution.num_stops;
        result_meta[2] = solution.total_cost;
        result_meta[3] = solution.remaining_fuel;
    } else {
        result_meta[0] = solution.status;
        result_meta[1] = 0;
        result_meta[2] = 0;
        result_meta[3] = 0;
    }

    fw_free_solution(&solution);
    free(stations);
    return ret;
}

/* ============================================================================
 * Refueling Optimization - With Segments (Variable Consumption)
 *
 * Stations: [id, distance, price] x num_stations
 * Segments: [start_distance, weight_lbs, mpg] x num_segments
 * ============================================================================ */

WASM_EXPORT
int wasm_solve_segments(
    double *stations_flat,      /* [id, distance, price] x num_stations */
    int num_stations,
    double *segments_flat,      /* [start_dist, weight, mpg] x num_segments */
    int num_segments,
    double total_distance,
    double tank_capacity,
    double current_fuel,
    double minimum_fuel,
    double min_purchase,        /* 0 = no minimum */
    double stop_cost,           /* 0 = no stop cost */
    double *result_purchases,
    double *result_meta)
{
    /* Validate inputs */
    if (!stations_flat || num_stations <= 0 ||
        !result_purchases || !result_meta) {
        return -1;
    }
    if (num_segments > 0 && !segments_flat) {
        return -1;
    }

    /* Build problem */
    FWRefuelProblem problem;
    memset(&problem, 0, sizeof(problem));

    problem.total_distance = total_distance;
    problem.tank_capacity = tank_capacity;
    problem.current_fuel = current_fuel;
    problem.minimum_fuel = minimum_fuel;
    problem.minimum_fuel_at_end = minimum_fuel;
    problem.min_purchase = min_purchase;
    problem.stop_cost = stop_cost;
    problem.num_stations = num_stations;

    /* Build stations */
    FWSnappedStation *stations = malloc((size_t)num_stations * sizeof(FWSnappedStation));
    if (!stations) return -1;

    for (int i = 0; i < num_stations; i++) {
        stations[i].station_id = (int)stations_flat[i * 3 + 0];
        stations[i].distance_from_start = stations_flat[i * 3 + 1];
        stations[i].price_per_gallon = stations_flat[i * 3 + 2];
        stations[i].perpendicular_distance = 0;
    }
    problem.stations = stations;

    /* Build segments */
    FWRouteSegment *segments = NULL;
    if (num_segments > 0) {
        segments = malloc((size_t)num_segments * sizeof(FWRouteSegment));
        if (!segments) {
            free(stations);
            return -1;
        }
        for (int i = 0; i < num_segments; i++) {
            segments[i].start_distance = segments_flat[i * 3 + 0];
            segments[i].cargo_weight_lbs = segments_flat[i * 3 + 1];
            segments[i].consumption_mpg = segments_flat[i * 3 + 2];
        }
        problem.num_segments = num_segments;
        problem.segments = segments;
    }

    /* Solve - use MILP if we have min_purchase or stop_cost */
    FWRefuelSolution solution;
    int ret;
    if (min_purchase > 0 || stop_cost > 0) {
        ret = fw_solve_refuel_milp(&problem, &solution);
    } else {
        ret = fw_solve_refuel_lp(&problem, &solution);
    }

    if (ret == 0 && solution.status == FW_STATUS_OPTIMAL) {
        for (int i = 0; i < num_stations; i++) {
            result_purchases[i] = solution.purchases[i];
        }
        result_meta[0] = solution.status;
        result_meta[1] = solution.num_stops;
        result_meta[2] = solution.total_cost;
        result_meta[3] = solution.remaining_fuel;
    } else {
        result_meta[0] = solution.status;
        result_meta[1] = 0;
        result_meta[2] = 0;
        result_meta[3] = 0;
    }

    fw_free_solution(&solution);
    free(stations);
    free(segments);
    return ret;
}

/* ============================================================================
 * Full Pipeline - Filter + Optimize
 * ============================================================================ */

WASM_EXPORT
int wasm_optimize_route(
    double *stations_flat,      /* [lat, lon, price, id] x num_stations */
    int num_stations,
    double *route_flat,         /* [lat, lon] x num_points */
    int num_points,
    double *segments_flat,      /* [start_dist, weight, mpg] x num_segments, or NULL */
    int num_segments,
    double tank_capacity,
    double current_fuel,
    double base_consumption_mpg,
    double minimum_fuel,
    double max_filter_distance,
    double *result_purchases,   /* Output: up to num_stations purchases */
    double *result_meta,        /* Output: [status, num_filtered, num_stops, total_cost, remaining_fuel, route_dist] */
    int *result_station_ids)    /* Output: filtered station IDs */
{
    /* Validate inputs */
    if (!stations_flat || num_stations <= 0 ||
        !route_flat || num_points < 2 ||
        !result_purchases || !result_meta || !result_station_ids) {
        return -1;
    }
    if (num_segments > 0 && !segments_flat) {
        return -1;
    }

    /* Build station array */
    FWStation *stations = malloc((size_t)num_stations * sizeof(FWStation));
    if (!stations) return -1;

    for (int i = 0; i < num_stations; i++) {
        stations[i].location.lat = stations_flat[i * 4 + 0];
        stations[i].location.lon = stations_flat[i * 4 + 1];
        stations[i].price_per_gallon = stations_flat[i * 4 + 2];
        stations[i].id = (int)stations_flat[i * 4 + 3];
        stations[i].name = NULL;
    }

    /* Build polyline */
    FWPolyline route;
    route.points = (FWCoord*)route_flat;
    route.count = num_points;

    /* Filter stations */
    FWSnappedStation *filtered = NULL;
    int filtered_count = 0;
    int ret = fw_filter_stations(stations, num_stations, &route, max_filter_distance,
                                  &filtered, &filtered_count);
    free(stations);

    if (ret != 0 || filtered_count == 0) {
        result_meta[0] = FW_STATUS_ERROR;
        result_meta[1] = 0;
        return ret != 0 ? ret : -1;
    }

    /* Calculate route distance */
    double route_dist = fw_polyline_length(&route);

    /* Build problem */
    FWRefuelProblem problem;
    memset(&problem, 0, sizeof(problem));
    problem.total_distance = route_dist;
    problem.tank_capacity = tank_capacity;
    problem.current_fuel = current_fuel;
    problem.base_consumption_mpg = base_consumption_mpg;
    problem.minimum_fuel = minimum_fuel;
    problem.minimum_fuel_at_end = minimum_fuel;
    problem.num_stations = filtered_count;
    problem.stations = filtered;

    /* Add segments if provided */
    FWRouteSegment *segments = NULL;
    if (num_segments > 0 && segments_flat) {
        segments = malloc((size_t)num_segments * sizeof(FWRouteSegment));
        if (!segments) {
            fw_free_snapped_stations(filtered);
            return -1;
        }
        for (int i = 0; i < num_segments; i++) {
            segments[i].start_distance = segments_flat[i * 3 + 0];
            segments[i].cargo_weight_lbs = segments_flat[i * 3 + 1];
            segments[i].consumption_mpg = segments_flat[i * 3 + 2];
        }
        problem.num_segments = num_segments;
        problem.segments = segments;
    }

    /* Solve */
    FWRefuelSolution solution;
    ret = fw_solve_refuel_lp(&problem, &solution);

    /* Copy results */
    result_meta[1] = filtered_count;
    result_meta[5] = route_dist;

    if (ret == 0 && solution.status == FW_STATUS_OPTIMAL) {
        result_meta[0] = solution.status;
        result_meta[2] = solution.num_stops;
        result_meta[3] = solution.total_cost;
        result_meta[4] = solution.remaining_fuel;

        for (int i = 0; i < filtered_count; i++) {
            result_purchases[i] = solution.purchases[i];
            result_station_ids[i] = filtered[i].station_id;
        }
    } else {
        result_meta[0] = solution.status;
        result_meta[2] = 0;
        result_meta[3] = 0;
        result_meta[4] = 0;
    }

    fw_free_solution(&solution);
    fw_free_snapped_stations(filtered);
    free(segments);
    return ret;
}
