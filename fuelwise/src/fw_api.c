/*
 * FuelWise Transport-Agnostic API Handler
 *
 * This implementation can be used by:
 * - HTTP servers (Mongoose)
 * - WASM modules
 * - Unix sockets
 * - Direct C calls
 *
 * Copyright (c) 2024. All rights reserved.
 */

#include "fw_api.h"
#include "fuelwise.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ============================================================================
 * API Context (currently stateless)
 * ============================================================================ */

struct FWAPIContext {
    int dummy;  /* Placeholder for future state */
};

/* ============================================================================
 * JSON Parsing Helpers (using sh_json)
 * ============================================================================ */

/* Parse a JSON array of points into a polyline */
static int parse_polyline(const ShJsonValue *arr, FWPolyline *polyline) {
    memset(polyline, 0, sizeof(FWPolyline));

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) return -1;

    size_t count = sh_json_array_len(arr);
    if (count == 0) return -1;

    polyline->points = calloc(count, sizeof(FWCoord));
    if (!polyline->points) return -1;
    polyline->count = (int)count;

    for (size_t i = 0; i < count; i++) {
        ShJsonValue *point = sh_json_array_get(arr, i);
        if (!point || sh_json_type(point) != SH_JSON_ARRAY) continue;

        /* Point is [lat, lon] */
        polyline->points[i].lat = sh_json_as_double(sh_json_array_get(point, 0), 0.0);
        polyline->points[i].lon = sh_json_as_double(sh_json_array_get(point, 1), 0.0);
    }

    return 0;
}

/* Parse a JSON array of route segments */
static int parse_segments(const ShJsonValue *arr, FWRouteSegment **segments, int *count) {
    *segments = NULL;
    *count = 0;

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) return 0;

    size_t seg_count = sh_json_array_len(arr);
    if (seg_count == 0) return 0;

    *segments = calloc(seg_count, sizeof(FWRouteSegment));
    if (!*segments) return -1;
    *count = (int)seg_count;

    for (size_t i = 0; i < seg_count; i++) {
        ShJsonValue *seg = sh_json_array_get(arr, i);
        if (!seg) continue;

        /* Parse segment fields with fallback keys */
        ShJsonValue *v;
        if ((v = sh_json_get(seg, "start_distance")) || (v = sh_json_get(seg, "start"))) {
            (*segments)[i].start_distance = sh_json_as_double(v, 0.0);
        }
        if ((v = sh_json_get(seg, "cargo_weight")) || (v = sh_json_get(seg, "weight"))) {
            (*segments)[i].cargo_weight = sh_json_as_double(v, 0.0);
        }
        if ((v = sh_json_get(seg, "consumption")) || (v = sh_json_get(seg, "mpg"))) {
            (*segments)[i].consumption = sh_json_as_double(v, 0.0);
        }
    }

    return 0;
}

/* Parse a JSON array of stations with lat/lon/price */
static int parse_stations_geo(const ShJsonValue *arr, FWStation **stations, int *count) {
    *stations = NULL;
    *count = 0;

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) return -1;

    size_t station_count = sh_json_array_len(arr);
    if (station_count == 0) return 0;

    *stations = calloc(station_count, sizeof(FWStation));
    if (!*stations) return -1;
    *count = (int)station_count;

    for (size_t i = 0; i < station_count; i++) {
        ShJsonValue *st = sh_json_array_get(arr, i);
        if (!st) continue;

        ShJsonValue *v;
        if ((v = sh_json_get(st, "id"))) {
            (*stations)[i].id = sh_json_as_int(v, (int)(i + 1));
        } else {
            (*stations)[i].id = (int)(i + 1);
        }
        (*stations)[i].location.lat = sh_json_as_double(sh_json_get(st, "lat"), 0.0);
        (*stations)[i].location.lon = sh_json_as_double(sh_json_get(st, "lon"), 0.0);
        (*stations)[i].price = sh_json_as_double(sh_json_get(st, "price"), 0.0);
        (*stations)[i].name = NULL;
    }

    return 0;
}

/* Parse snapped stations for solve request */
static int parse_snapped_stations(const ShJsonValue *arr, FWSnappedStation **stations, int *count) {
    *stations = NULL;
    *count = 0;

    if (!arr || sh_json_type(arr) != SH_JSON_ARRAY) return -1;

    size_t station_count = sh_json_array_len(arr);
    if (station_count == 0) return -1;

    *stations = calloc(station_count, sizeof(FWSnappedStation));
    if (!*stations) return -1;
    *count = (int)station_count;

    for (size_t i = 0; i < station_count; i++) {
        ShJsonValue *st = sh_json_array_get(arr, i);
        if (!st) continue;

        ShJsonValue *v;
        if ((v = sh_json_get(st, "station_id")) || (v = sh_json_get(st, "id"))) {
            (*stations)[i].station_id = sh_json_as_int(v, 0);
        }
        if ((v = sh_json_get(st, "distance_from_start")) || (v = sh_json_get(st, "distance"))) {
            (*stations)[i].distance_from_start = sh_json_as_double(v, 0.0);
        }
        (*stations)[i].price = sh_json_as_double(sh_json_get(st, "price"), 0.0);
    }

    return 0;
}

/* Parse a solve request from JSON */
static int parse_solve_request(const ShJsonValue *root, FWRefuelProblem *problem) {
    memset(problem, 0, sizeof(FWRefuelProblem));

    if (!root || sh_json_type(root) != SH_JSON_OBJECT) return -1;

    /* Parse scalar fields */
    problem->total_distance = sh_json_as_double(sh_json_get(root, "total_distance"), 0.0);
    problem->tank_capacity = sh_json_as_double(sh_json_get(root, "tank_capacity"), 0.0);
    problem->current_fuel = sh_json_as_double(sh_json_get(root, "current_fuel"), 0.0);

    ShJsonValue *v;
    if ((v = sh_json_get(root, "consumption")) || (v = sh_json_get(root, "consumption_mpg"))) {
        problem->base_consumption = sh_json_as_double(v, 0.0);
    }

    problem->minimum_fuel = sh_json_as_double(sh_json_get(root, "minimum_fuel"), 0.0);

    if ((v = sh_json_get(root, "minimum_fuel_at_end"))) {
        problem->minimum_fuel_at_end = sh_json_as_double(v, problem->minimum_fuel);
    } else {
        problem->minimum_fuel_at_end = problem->minimum_fuel;
    }

    problem->min_purchase = sh_json_as_double(sh_json_get(root, "min_purchase"), 0.0);
    problem->stop_cost = sh_json_as_double(sh_json_get(root, "stop_cost"), 0.0);

    /* Parse segments array (optional) */
    ShJsonValue *segments_arr = sh_json_get(root, "segments");
    if (segments_arr && sh_json_type(segments_arr) == SH_JSON_ARRAY) {
        FWRouteSegment *segments = NULL;
        int num_segments = 0;
        if (parse_segments(segments_arr, &segments, &num_segments) == 0 && num_segments > 0) {
            problem->segments = segments;
            problem->num_segments = num_segments;
        }
    }

    /* Parse stations array (required) */
    ShJsonValue *stations_arr = sh_json_get(root, "stations");
    if (!stations_arr || sh_json_type(stations_arr) != SH_JSON_ARRAY) {
        return -1;
    }

    if (parse_snapped_stations(stations_arr, &problem->stations, &problem->num_stations) != 0) {
        return -1;
    }

    return 0;
}

/* Free parsed problem resources */
static void free_problem(FWRefuelProblem *problem) {
    free(problem->stations);
    free(problem->segments);
}

/* ============================================================================
 * Core Processing Functions
 * ============================================================================ */

/* Process a solve request - returns malloc'd response string */
static char *process_solve(const char *body, size_t body_len, int *status_code) {
    *status_code = 200;

    /* Create arena for JSON parsing */
    SHArena *arena = sh_arena_create(body_len * 4 + 4096);
    if (!arena) {
        *status_code = 500;
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    /* Parse JSON */
    ShJsonValue *root = NULL;
    ShJsonStatus json_status = sh_json_parse(body, body_len, arena, &root);
    if (json_status != SH_JSON_OK) {
        sh_arena_free(arena);
        *status_code = 400;
        char *resp = malloc(256);
        if (resp) snprintf(resp, 256, "{\"error\": \"JSON parse error: %s\"}\n",
                          sh_json_status_str(json_status));
        return resp;
    }

    /* Parse request */
    FWRefuelProblem problem;
    if (parse_solve_request(root, &problem) != 0) {
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid request format\"}\n");
    }

    sh_arena_free(arena);  /* Done with JSON, problem has copies */

    /* Validate problem */
    char error_msg[256];
    if (!fw_validate_problem(&problem, error_msg, sizeof(error_msg))) {
        *status_code = 400;
        char *resp = malloc(512);
        if (resp) snprintf(resp, 512, "{\"error\": \"%s\"}\n", error_msg);
        free_problem(&problem);
        return resp;
    }

    /* Solve */
    FWRefuelSolution solution;
    int use_milp = (problem.min_purchase > 0 || problem.stop_cost > 0);

    int ret;
    if (use_milp) {
        ret = fw_solve_refuel_milp(&problem, &solution);
    } else {
        ret = fw_solve_refuel_lp(&problem, &solution);
    }

    if (ret != 0 || solution.status != FW_STATUS_OPTIMAL) {
        *status_code = 422;
        char *resp = malloc(256);
        if (resp) snprintf(resp, 256, "{\"error\": \"%s\"}\n", fw_status_string(solution.status));
        fw_free_solution(&solution);
        free_problem(&problem);
        return resp;
    }

    /* Build response */
    size_t per_station = 256;
    size_t base_size = 2048;
    if (problem.num_stations < 0 ||
        (size_t)problem.num_stations > (SIZE_MAX - base_size) / per_station) {
        *status_code = 500;
        fw_free_solution(&solution);
        free_problem(&problem);
        return strdup("{\"error\": \"Too many stations for response buffer\"}\n");
    }
    size_t buf_size = base_size + (size_t)problem.num_stations * per_station;
    char *response = malloc(buf_size);
    if (!response) {
        *status_code = 500;
        fw_free_solution(&solution);
        free_problem(&problem);
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    size_t pos = 0;
    int n = snprintf(response + pos, buf_size - pos,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"num_stops\": %d,\n"
        "  \"total_cost\": %.2f,\n"
        "  \"gross_cost\": %.2f,\n"
        "  \"remaining_fuel\": %.2f,\n"
        "  \"stops\": [",
        fw_status_string(solution.status),
        solution.num_stops,
        solution.total_cost,
        solution.gross_cost,
        solution.remaining_fuel);
    if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;

    int first = 1;
    for (int i = 0; i < problem.num_stations; i++) {
        if (solution.purchases[i] > 0.001) {
            if (!first) {
                n = snprintf(response + pos, buf_size - pos, ",");
                if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
            }
            first = 0;
            n = snprintf(response + pos, buf_size - pos,
                "\n    {"
                "\"station_id\": %d, "
                "\"gallons\": %.2f, "
                "\"cost\": %.2f"
                "}",
                problem.stations[i].station_id,
                solution.purchases[i],
                solution.purchases[i] * problem.stations[i].price);
            if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
    }

    snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    fw_free_solution(&solution);
    free_problem(&problem);
    return response;
}

/* Process a filter request - returns malloc'd response string */
static char *process_filter(const char *body, size_t body_len, int *status_code) {
    *status_code = 200;

    /* Create arena for JSON parsing */
    SHArena *arena = sh_arena_create(body_len * 4 + 4096);
    if (!arena) {
        *status_code = 500;
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    /* Parse JSON */
    ShJsonValue *root = NULL;
    ShJsonStatus json_status = sh_json_parse(body, body_len, arena, &root);
    if (json_status != SH_JSON_OK) {
        sh_arena_free(arena);
        *status_code = 400;
        char *resp = malloc(256);
        if (resp) snprintf(resp, 256, "{\"error\": \"JSON parse error: %s\"}\n",
                          sh_json_status_str(json_status));
        return resp;
    }

    /* Parse stations array */
    ShJsonValue *stations_json = sh_json_get(root, "stations");
    if (!stations_json) {
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'stations' array\"}\n");
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid stations format\"}\n");
    }

    /* Parse route polyline */
    ShJsonValue *route_json = sh_json_get(root, "route");
    if (!route_json) {
        free(stations);
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'route' array\"}\n");
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid route format\"}\n");
    }

    /* Parse max_distance (default 5 miles) */
    double max_distance = sh_json_as_double(sh_json_get(root, "max_distance"), 5.0);

    sh_arena_free(arena);  /* Done with JSON */

    /* Filter stations */
    FWSnappedStation *filtered = NULL;
    int filtered_count = 0;
    int ret = fw_filter_stations(stations, num_stations, &route, max_distance,
                                  &filtered, &filtered_count);

    free(stations);
    free(route.points);

    if (ret != 0) {
        *status_code = 500;
        return strdup("{\"error\": \"Filter operation failed\"}\n");
    }

    /* Build response */
    size_t per_station = 256;
    size_t base_size = 1024;
    if (filtered_count < 0 ||
        (size_t)filtered_count > (SIZE_MAX - base_size) / per_station) {
        fw_free_snapped_stations(filtered);
        *status_code = 500;
        return strdup("{\"error\": \"Too many stations for response buffer\"}\n");
    }
    size_t buf_size = base_size + (size_t)filtered_count * per_station;
    char *response = malloc(buf_size);
    if (!response) {
        fw_free_snapped_stations(filtered);
        *status_code = 500;
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    size_t pos = 0;
    int n = snprintf(response + pos, buf_size - pos,
        "{\n"
        "  \"count\": %d,\n"
        "  \"stations\": [",
        filtered_count);
    if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;

    for (int i = 0; i < filtered_count; i++) {
        if (i > 0) {
            n = snprintf(response + pos, buf_size - pos, ",");
            if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
        n = snprintf(response + pos, buf_size - pos,
            "\n    {"
            "\"station_id\": %d, "
            "\"distance_from_start\": %.2f, "
            "\"perpendicular_distance\": %.3f, "
            "\"price\": %.3f, "
            "\"snap_point\": [%.6f, %.6f]"
            "}",
            filtered[i].station_id,
            filtered[i].distance_from_start,
            filtered[i].perpendicular_distance,
            filtered[i].price,
            filtered[i].snap_point.lat,
            filtered[i].snap_point.lon);
        if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }

    snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    fw_free_snapped_stations(filtered);
    return response;
}

/* Process an optimize request - returns malloc'd response string */
static char *process_optimize(const char *body, size_t body_len, int *status_code) {
    *status_code = 200;

    /* Create arena for JSON parsing */
    SHArena *arena = sh_arena_create(body_len * 4 + 4096);
    if (!arena) {
        *status_code = 500;
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    /* Parse JSON */
    ShJsonValue *root = NULL;
    ShJsonStatus json_status = sh_json_parse(body, body_len, arena, &root);
    if (json_status != SH_JSON_OK) {
        sh_arena_free(arena);
        *status_code = 400;
        char *resp = malloc(256);
        if (resp) snprintf(resp, 256, "{\"error\": \"JSON parse error: %s\"}\n",
                          sh_json_status_str(json_status));
        return resp;
    }

    /* Parse stations array */
    ShJsonValue *stations_json = sh_json_get(root, "stations");
    if (!stations_json) {
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'stations' array\"}\n");
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid stations format\"}\n");
    }

    /* Parse route polyline */
    ShJsonValue *route_json = sh_json_get(root, "route");
    if (!route_json) {
        free(stations);
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'route' array\"}\n");
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        sh_arena_free(arena);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid route format\"}\n");
    }

    /* Parse config with defaults */
    double tank_capacity = sh_json_as_double(sh_json_get(root, "tank_capacity"), 100.0);
    double current_fuel = sh_json_as_double(sh_json_get(root, "current_fuel"), 50.0);
    double min_fuel = sh_json_as_double(sh_json_get(root, "minimum_fuel"), 25.0);
    double max_distance = sh_json_as_double(sh_json_get(root, "max_distance"), 5.0);
    double min_purchase = sh_json_as_double(sh_json_get(root, "min_purchase"), 0.0);
    double stop_cost = sh_json_as_double(sh_json_get(root, "stop_cost"), 0.0);
    double remaining_fuel_value = sh_json_as_double(sh_json_get(root, "remaining_fuel_value"), 0.0);

    /* Consumption with fallback key */
    double consumption = 6.5;
    ShJsonValue *cons_v;
    if ((cons_v = sh_json_get(root, "consumption")) || (cons_v = sh_json_get(root, "consumption_mpg"))) {
        consumption = sh_json_as_double(cons_v, 6.5);
    }

    /* Parse segments (optional) */
    FWRouteSegment *segments = NULL;
    int num_segments = 0;
    ShJsonValue *segments_json = sh_json_get(root, "segments");
    if (segments_json && sh_json_type(segments_json) == SH_JSON_ARRAY) {
        parse_segments(segments_json, &segments, &num_segments);
    }

    sh_arena_free(arena);  /* Done with JSON */

    /* Filter stations to route */
    FWSnappedStation *filtered = NULL;
    int filtered_count = 0;
    int ret = fw_filter_stations(stations, num_stations, &route, max_distance,
                                  &filtered, &filtered_count);

    free(stations);

    if (ret != 0 || filtered_count == 0) {
        free(route.points);
        if (filtered) fw_free_snapped_stations(filtered);
        free(segments);
        *status_code = 422;
        return strdup(filtered_count == 0 ?
            "{\"error\": \"No stations found within distance of route\"}\n" :
            "{\"error\": \"Filter failed\"}\n");
    }

    /* Build refueling problem */
    FWRefuelProblem problem;
    memset(&problem, 0, sizeof(problem));
    problem.total_distance = fw_polyline_length(&route);
    problem.tank_capacity = tank_capacity;
    problem.current_fuel = current_fuel;
    problem.base_consumption = consumption;
    problem.minimum_fuel = min_fuel;
    problem.minimum_fuel_at_end = min_fuel;
    problem.min_purchase = min_purchase;
    problem.stop_cost = stop_cost;
    problem.remaining_fuel_value = remaining_fuel_value;
    problem.num_stations = filtered_count;
    problem.stations = filtered;

    if (num_segments > 0 && segments) {
        problem.num_segments = num_segments;
        problem.segments = segments;
    }

    free(route.points);

    /* Validate */
    char error_msg[256];
    if (!fw_validate_problem(&problem, error_msg, sizeof(error_msg))) {
        fw_free_snapped_stations(filtered);
        free(segments);
        *status_code = 400;
        char *resp = malloc(512);
        if (resp) snprintf(resp, 512, "{\"error\": \"%s\"}\n", error_msg);
        return resp;
    }

    /* Solve */
    FWRefuelSolution solution;
    int use_milp = (min_purchase > 0 || stop_cost > 0);
    if (use_milp) {
        ret = fw_solve_refuel_milp(&problem, &solution);
    } else {
        ret = fw_solve_refuel_lp(&problem, &solution);
    }

    if (ret != 0 || solution.status != FW_STATUS_OPTIMAL) {
        fw_free_snapped_stations(filtered);
        free(segments);
        *status_code = 422;
        char *resp = malloc(256);
        if (resp) snprintf(resp, 256, "{\"error\": \"%s\"}\n", fw_status_string(solution.status));
        fw_free_solution(&solution);
        return resp;
    }

    /* Build response */
    size_t per_station = 256;
    size_t base_size = 2048;
    if (filtered_count < 0 ||
        (size_t)filtered_count > (SIZE_MAX - base_size) / per_station) {
        fw_free_snapped_stations(filtered);
        fw_free_solution(&solution);
        free(segments);
        *status_code = 500;
        return strdup("{\"error\": \"Too many stations for response buffer\"}\n");
    }
    size_t buf_size = base_size + (size_t)filtered_count * per_station;
    char *response = malloc(buf_size);
    if (!response) {
        fw_free_snapped_stations(filtered);
        fw_free_solution(&solution);
        free(segments);
        *status_code = 500;
        return strdup("{\"error\": \"Memory allocation failed\"}\n");
    }

    size_t pos = 0;
    int n = snprintf(response + pos, buf_size - pos,
        "{\n"
        "  \"status\": \"%s\",\n"
        "  \"route_distance\": %.2f,\n"
        "  \"stations_filtered\": %d,\n"
        "  \"num_stops\": %d,\n"
        "  \"total_cost\": %.2f,\n"
        "  \"gross_cost\": %.2f,\n"
        "  \"remaining_fuel\": %.2f,\n"
        "  \"stops\": [",
        fw_status_string(solution.status),
        problem.total_distance,
        filtered_count,
        solution.num_stops,
        solution.total_cost,
        solution.gross_cost,
        solution.remaining_fuel);
    if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;

    int first = 1;
    for (int i = 0; i < filtered_count; i++) {
        if (solution.purchases[i] > 0.001) {
            if (!first) {
                n = snprintf(response + pos, buf_size - pos, ",");
                if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
            }
            first = 0;
            n = snprintf(response + pos, buf_size - pos,
                "\n    {"
                "\"station_id\": %d, "
                "\"distance_from_start\": %.2f, "
                "\"gallons\": %.2f, "
                "\"cost\": %.2f"
                "}",
                filtered[i].station_id,
                filtered[i].distance_from_start,
                solution.purchases[i],
                solution.purchases[i] * filtered[i].price);
            if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
    }

    snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    fw_free_snapped_stations(filtered);
    fw_free_solution(&solution);
    free(segments);
    return response;
}

/* Process health request */
static char *process_health(int *status_code) {
    *status_code = 200;
    char *response = malloc(256);
    if (!response) return strdup("{\"error\": \"Memory allocation failed\"}\n");

    snprintf(response, 256,
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"fuelwise-api\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        fw_version());
    return response;
}

/* Process stats request */
static char *process_stats(int *status_code) {
    *status_code = 200;
    char *response = malloc(512);
    if (!response) return strdup("{\"error\": \"Memory allocation failed\"}\n");

    snprintf(response, 512,
        "{\n"
        "  \"service\": \"fuelwise-api\",\n"
        "  \"version\": \"%s\",\n"
        "  \"work_queue\": {\n"
        "    \"enabled\": false\n"
        "  },\n"
        "  \"rate_limit\": {\n"
        "    \"enabled\": false\n"
        "  }\n"
        "}\n",
        fw_version());
    return response;
}

/* ============================================================================
 * Public API Functions
 * ============================================================================ */

FWAPIContext *fw_api_create(void) {
    FWAPIContext *ctx = calloc(1, sizeof(FWAPIContext));
    return ctx;
}

void fw_api_free(FWAPIContext *ctx) {
    free(ctx);
}

void fw_api_response_free(FWAPIResponse *resp) {
    if (resp) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

int fw_api_handle(FWAPIContext *ctx, const FWAPIRequest *req, FWAPIResponse *resp) {
    (void)ctx;  /* Currently stateless */

    if (!req || !resp) {
        return -1;
    }

    /* Initialize response */
    memset(resp, 0, sizeof(FWAPIResponse));
    resp->content_type = "application/json";

    /* Check for NULL path */
    if (!req->path) {
        resp->status_code = 400;
        resp->body = strdup("{\"error\": \"Missing request path\"}\n");
        resp->body_len = resp->body ? strlen(resp->body) : 0;
        return 0;
    }

    int status_code = 200;
    char *response_body = NULL;

    /* Route based on path */
    if (strcmp(req->path, "/api/v1/health") == 0) {
        response_body = process_health(&status_code);
    }
    else if (strcmp(req->path, "/api/v1/stats") == 0) {
        response_body = process_stats(&status_code);
    }
    else if (strcmp(req->path, "/api/v1/solve") == 0) {
        if (!req->body || req->body_len == 0) {
            status_code = 400;
            response_body = strdup("{\"error\": \"Missing request body\"}\n");
        } else {
            response_body = process_solve(req->body, req->body_len, &status_code);
        }
    }
    else if (strcmp(req->path, "/api/v1/filter") == 0) {
        if (!req->body || req->body_len == 0) {
            status_code = 400;
            response_body = strdup("{\"error\": \"Missing request body\"}\n");
        } else {
            response_body = process_filter(req->body, req->body_len, &status_code);
        }
    }
    else if (strcmp(req->path, "/api/v1/optimize") == 0) {
        if (!req->body || req->body_len == 0) {
            status_code = 400;
            response_body = strdup("{\"error\": \"Missing request body\"}\n");
        } else {
            response_body = process_optimize(req->body, req->body_len, &status_code);
        }
    }
    else {
        status_code = 404;
        response_body = strdup("{\"error\": \"Not found\"}\n");
    }

    resp->status_code = status_code;
    resp->body = response_body;
    resp->body_len = response_body ? strlen(response_body) : 0;

    return 0;
}
