/*
 * FuelWise - Truck Refueling Optimization Library
 * Unified API Implementation
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "fuelwise.h"

/* ============================================================================
 * Version Information
 * ============================================================================ */

static const char* VERSION_STRING = "1.0.0";

const char* fw_version(void)
{
    return VERSION_STRING;
}

/* ============================================================================
 * Status Messages
 * ============================================================================ */

const char* fw_status_string(FWStatus status)
{
    switch (status) {
        case FW_STATUS_OPTIMAL:        return "OPTIMAL";
        case FW_STATUS_INFEASIBLE:     return "INFEASIBLE";
        case FW_STATUS_UNBOUNDED:      return "UNBOUNDED";
        case FW_STATUS_ERROR:          return "ERROR";
        case FW_STATUS_ITERATION_LIMIT: return "ITERATION_LIMIT";
        case FW_STATUS_TIME_LIMIT:     return "TIME_LIMIT";
        default:                       return "UNKNOWN";
    }
}

/* ============================================================================
 * Default Configuration
 * ============================================================================ */

void fw_default_filter_config(FWFilterConfig *config)
{
    if (!config) return;

    config->max_distance = 8000.0;          /* 8 km in meters */
    config->max_dedup_distance = 16000.0;   /* 16 km in meters */
    config->min_repeat_distance = 250.0;    /* 250 meters */
    config->dedup_strategy = FW_DEDUP_CLOSEST;
}

/* ============================================================================
 * High-Level Optimization API
 * ============================================================================ */

int fw_optimize(
    const FWOptimizeRequest *request,
    FWOptimizeResponse *response)
{
    if (!request || !response) return -1;

    /* Initialize response */
    memset(response, 0, sizeof(FWOptimizeResponse));

    /* Step 1: Filter and snap stations to route */
    FWSnappedStation *filtered = NULL;
    int filtered_count = 0;

    int ret;
    if (request->filter_config.max_distance > 0) {
        ret = fw_filter_stations_two_step(
            request->stations,
            request->num_stations,
            request->route,
            request->overview_route,
            &request->filter_config,
            &filtered,
            &filtered_count
        );
    } else {
        /* Use simple filtering */
        FWFilterConfig default_config;
        fw_default_filter_config(&default_config);
        ret = fw_filter_stations_two_step(
            request->stations,
            request->num_stations,
            request->route,
            request->overview_route,
            &default_config,
            &filtered,
            &filtered_count
        );
    }

    if (ret != 0) {
        response->status = FW_STATUS_ERROR;
        return -1;
    }

    /* Store filtered stations in response */
    response->filtered_stations = filtered;
    response->num_filtered_stations = filtered_count;

    /* Calculate route distance */
    response->total_distance = fw_polyline_length(request->route);

    /* Step 2: Build refueling problem */
    FWRefuelProblem problem = {
        .total_distance = response->total_distance,
        .num_segments = request->num_segments,
        .segments = request->segments,
        .base_consumption = request->consumption,
        .tank_capacity = request->tank_capacity,
        .current_fuel = request->current_fuel,
        .minimum_fuel = request->minimum_fuel,
        .minimum_fuel_at_end = (request->minimum_fuel_at_end > 0) ?
                                request->minimum_fuel_at_end : request->minimum_fuel,
        .num_stations = filtered_count,
        .stations = filtered,
        .min_purchase = request->min_purchase,
        .stop_cost = request->stop_cost,
        .remaining_fuel_value = request->remaining_fuel_value
    };

    /* Calculate total fuel consumed */
    response->total_fuel_consumed = fw_calc_total_fuel_consumed(&problem);

    /* Step 3: Solve */
    FWRefuelSolution solution;
    if (request->use_milp) {
        ret = fw_solve_refuel_milp(&problem, &solution);
    } else {
        ret = fw_solve_refuel_lp(&problem, &solution);
    }

    if (ret != 0) {
        response->status = solution.status;
        return -1;
    }

    /* Copy solution to response */
    response->status = solution.status;
    response->num_stops = solution.num_stops;
    response->total_cost = solution.total_cost;
    response->gross_cost = solution.gross_cost;
    response->remaining_fuel = solution.remaining_fuel;
    response->purchases = solution.purchases;
    response->stop_flags = solution.stop_flags;

    /* Don't free solution arrays - they're now owned by response */
    return 0;
}

void fw_free_response(FWOptimizeResponse *response)
{
    if (response) {
        fw_free_snapped_stations(response->filtered_stations);
        free(response->purchases);
        free(response->stop_flags);
        memset(response, 0, sizeof(FWOptimizeResponse));
    }
}

/* ============================================================================
 * Convenience Functions
 * ============================================================================ */

int fw_optimize_simple(
    const FWStation *stations,
    int num_stations,
    const FWPolyline *route,
    double tank_capacity,
    double current_fuel,
    double consumption,
    double min_fuel,
    FWRefuelSolution *solution)
{
    if (!solution) return -1;

    FWFilterConfig filter_config;
    fw_default_filter_config(&filter_config);

    FWOptimizeRequest request = {
        .stations = stations,
        .num_stations = num_stations,
        .route = route,
        .overview_route = NULL,
        .filter_config = filter_config,
        .tank_capacity = tank_capacity,
        .current_fuel = current_fuel,
        .consumption = consumption,
        .minimum_fuel = min_fuel,
        .minimum_fuel_at_end = min_fuel,
        .min_purchase = 0.0,
        .stop_cost = 0.0,
        .remaining_fuel_value = 0.0,
        .num_segments = 0,
        .segments = NULL,
        .use_milp = 0,
        .verbose = 0
    };

    FWOptimizeResponse response;
    int ret = fw_optimize(&request, &response);

    if (ret == 0 && response.status == FW_STATUS_OPTIMAL) {
        /* Copy solution data */
        solution->status = response.status;
        solution->num_stops = response.num_stops;
        solution->total_cost = response.total_cost;
        solution->gross_cost = response.gross_cost;
        solution->remaining_fuel = response.remaining_fuel;

        /* Transfer ownership of arrays */
        solution->purchases = response.purchases;
        solution->stop_flags = response.stop_flags;
        response.purchases = NULL;
        response.stop_flags = NULL;
    } else {
        solution->status = response.status;
    }

    /* Free response (but not the arrays we transferred) */
    fw_free_snapped_stations(response.filtered_stations);
    free(response.purchases);
    free(response.stop_flags);

    return ret;
}

/* ============================================================================
 * JSON Serialization (Basic Implementation)
 * ============================================================================ */

char* fw_solution_to_json(const FWRefuelSolution *solution)
{
    if (!solution) return NULL;

    /* Estimate buffer size */
    int buf_size = 1024;
    char *json = malloc(buf_size);
    if (!json) return NULL;

    int pos = 0;
    pos += snprintf(json + pos, buf_size - pos,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"status_code\": %d,\n"
        "  \"num_stops\": %d,\n"
        "  \"total_cost\": %.2f,\n"
        "  \"gross_cost\": %.2f,\n"
        "  \"remaining_fuel\": %.2f\n"
        "}\n",
        fw_status_string(solution->status),
        (int)solution->status,
        solution->num_stops,
        solution->total_cost,
        solution->gross_cost,
        solution->remaining_fuel
    );

    /*
     * Note: The purchases array is not serialized here because FWRefuelSolution
     * does not store num_stations. Use fw_response_to_json() for full details
     * including per-station purchases.
     */

    return json;
}

char* fw_response_to_json(const FWOptimizeResponse *response)
{
    if (!response) return NULL;

    /* Estimate buffer size based on number of stations */
    int buf_size = 2048 + response->num_filtered_stations * 128;
    char *json = malloc(buf_size);
    if (!json) return NULL;

    int pos = 0;
    pos += snprintf(json + pos, buf_size - pos,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"status_code\": %d,\n"
        "  \"total_distance\": %.2f,\n"
        "  \"total_fuel_consumed\": %.2f,\n"
        "  \"num_stops\": %d,\n"
        "  \"total_cost\": %.2f,\n"
        "  \"gross_cost\": %.2f,\n"
        "  \"remaining_fuel\": %.2f,\n"
        "  \"num_filtered_stations\": %d,\n"
        "  \"stops\": [",
        fw_status_string(response->status),
        (int)response->status,
        response->total_distance,
        response->total_fuel_consumed,
        response->num_stops,
        response->total_cost,
        response->gross_cost,
        response->remaining_fuel,
        response->num_filtered_stations
    );

    /* Add stops array */
    int first = 1;
    for (int i = 0; i < response->num_filtered_stations; i++) {
        if (response->purchases && response->purchases[i] > 0.001) {
            if (!first) {
                pos += snprintf(json + pos, buf_size - pos, ",");
            }
            first = 0;

            pos += snprintf(json + pos, buf_size - pos,
                "\n    {\n"
                "      \"station_id\": %d,\n"
                "      \"distance_from_start\": %.2f,\n"
                "      \"price\": %.4f,\n"
                "      \"liters\": %.2f,\n"
                "      \"cost\": %.2f\n"
                "    }",
                response->filtered_stations[i].station_id,
                response->filtered_stations[i].distance_from_start,
                response->filtered_stations[i].price,
                response->purchases[i],
                response->purchases[i] * response->filtered_stations[i].price
            );
        }
    }

    pos += snprintf(json + pos, buf_size - pos, "\n  ]\n}\n");

    return json;
}

int fw_problem_from_json(const char *json, FWRefuelProblem *problem)
{
    /* Basic JSON parsing - in production use a proper JSON library */
    (void)json;
    (void)problem;
    /* TODO: Implement JSON parsing */
    return -1;
}

int fw_request_from_json(const char *json, FWOptimizeRequest *request)
{
    /* Basic JSON parsing - in production use a proper JSON library */
    (void)json;
    (void)request;
    /* TODO: Implement JSON parsing */
    return -1;
}

void fw_free_json(char *json)
{
    free(json);
}
