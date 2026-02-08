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
 * JSON Parsing Helpers
 * ============================================================================ */

/* Skip whitespace */
static const char* skip_ws(const char *s) {
    while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
    return s;
}

/* Parse a double from JSON */
static double parse_double(const char **p) {
    const char *s = skip_ws(*p);
    char *end;
    double val = strtod(s, &end);
    *p = end;
    return val;
}

/* Parse an integer from JSON */
static int parse_int(const char **p) {
    const char *s = skip_ws(*p);
    char *end;
    int val = (int)strtol(s, &end, 10);
    *p = end;
    return val;
}

/* Find a key in a JSON object and return pointer to value.
 * Note: Key must be < 250 chars to fit in search buffer.
 */
static const char* find_json_key(const char *json, const char *key) {
    if (!json || !key) return NULL;

    /* Validate key length to prevent truncation */
    size_t key_len = strnlen(key, 256);
    if (key_len >= 250) return NULL;

    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *p = strstr(json, search);
    if (!p) return NULL;

    p += strlen(search);
    p = skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    return skip_ws(p);
}

/* Parse a JSON array of points into a polyline */
static int parse_polyline(const char *json_array, FWPolyline *polyline) {
    memset(polyline, 0, sizeof(FWPolyline));

    const char *p = skip_ws(json_array);
    if (*p != '[') return -1;
    p++;

    /* Count points - each point is [lat, lon] */
    int count = 0;
    int depth = 0;
    const char *scan = p;
    while (*scan) {
        if (*scan == '[') {
            if (depth == 0) count++;
            depth++;
        } else if (*scan == ']') {
            depth--;
            if (depth < 0) break;
        }
        scan++;
    }

    if (count == 0) return -1;

    /* Allocate points */
    polyline->points = calloc((size_t)count, sizeof(FWCoord));
    if (!polyline->points) return -1;
    polyline->num_points = count;

    /* Parse each [lat, lon] pair */
    for (int i = 0; i < count; i++) {
        p = skip_ws(p);
        if (*p != '[') break;
        p++;  /* skip '[' */

        polyline->points[i].lat = parse_double(&p);
        p = skip_ws(p);
        if (*p == ',') p++;
        polyline->points[i].lon = parse_double(&p);

        /* Find closing bracket */
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    return 0;
}

/* Parse a JSON array of route segments */
static int parse_segments(const char *json_array, FWRouteSegment **segments, int *count) {
    const char *p = skip_ws(json_array);
    if (*p != '[') return -1;

    /* Count segments */
    int seg_count = 0;
    int depth = 0;
    const char *scan = p + 1;
    while (*scan) {
        if (*scan == '{') {
            if (depth == 0) seg_count++;
            depth++;
        } else if (*scan == '}') {
            depth--;
        } else if (*scan == ']' && depth == 0) {
            break;
        }
        scan++;
    }

    if (seg_count == 0) {
        *segments = NULL;
        *count = 0;
        return 0;
    }

    /* Allocate segments */
    *segments = calloc((size_t)seg_count, sizeof(FWRouteSegment));
    if (!*segments) return -1;
    *count = seg_count;

    /* Parse each segment */
    p++;  /* skip '[' */
    for (int i = 0; i < seg_count; i++) {
        p = skip_ws(p);
        if (*p != '{') break;

        const char *obj_start = p;
        const char *obj_end = strchr(p, '}');
        if (!obj_end) break;

        /* Parse segment fields */
        const char *sp;
        if ((sp = find_json_key(obj_start, "start_distance"))) {
            (*segments)[i].start_distance = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "start"))) {
            (*segments)[i].start_distance = parse_double(&sp);
        }
        if ((sp = find_json_key(obj_start, "cargo_weight_lbs"))) {
            (*segments)[i].cargo_weight_lbs = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "weight"))) {
            (*segments)[i].cargo_weight_lbs = parse_double(&sp);
        }
        if ((sp = find_json_key(obj_start, "consumption_mpg"))) {
            (*segments)[i].consumption_mpg = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "mpg"))) {
            (*segments)[i].consumption_mpg = parse_double(&sp);
        }

        p = obj_end + 1;
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    return 0;
}

/* Parse a JSON array of stations with lat/lon/price */
static int parse_stations_geo(const char *json_array, FWStation **stations, int *count) {
    const char *p = skip_ws(json_array);
    if (*p != '[') return -1;

    /* Count stations */
    int station_count = 0;
    int depth = 0;
    const char *scan = p + 1;
    while (*scan) {
        if (*scan == '{') {
            if (depth == 0) station_count++;
            depth++;
        } else if (*scan == '}') {
            depth--;
        } else if (*scan == ']' && depth == 0) {
            break;
        }
        scan++;
    }

    if (station_count == 0) {
        *stations = NULL;
        *count = 0;
        return 0;
    }

    /* Allocate stations */
    *stations = calloc((size_t)station_count, sizeof(FWStation));
    if (!*stations) return -1;
    *count = station_count;

    /* Parse each station */
    p++;  /* skip '[' */
    for (int i = 0; i < station_count; i++) {
        p = skip_ws(p);
        if (*p != '{') break;

        const char *obj_start = p;
        const char *obj_end = strchr(p, '}');
        if (!obj_end) break;

        /* Parse station fields */
        const char *sp;
        if ((sp = find_json_key(obj_start, "id"))) {
            (*stations)[i].id = parse_int(&sp);
        } else {
            (*stations)[i].id = i + 1;
        }
        if ((sp = find_json_key(obj_start, "lat"))) {
            (*stations)[i].location.lat = parse_double(&sp);
        }
        if ((sp = find_json_key(obj_start, "lon"))) {
            (*stations)[i].location.lon = parse_double(&sp);
        }
        if ((sp = find_json_key(obj_start, "price"))) {
            (*stations)[i].price_per_gallon = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "price_per_gallon"))) {
            (*stations)[i].price_per_gallon = parse_double(&sp);
        }
        (*stations)[i].name = NULL;

        p = obj_end + 1;
        p = skip_ws(p);
        if (*p == ',') p++;
    }

    return 0;
}

/* Parse a solve request from JSON */
static int parse_solve_request(const char *json, FWRefuelProblem *problem) {
    memset(problem, 0, sizeof(FWRefuelProblem));

    const char *p;

    /* Parse scalar fields */
    if ((p = find_json_key(json, "total_distance"))) {
        problem->total_distance = parse_double(&p);
    }
    if ((p = find_json_key(json, "tank_capacity"))) {
        problem->tank_capacity = parse_double(&p);
    }
    if ((p = find_json_key(json, "current_fuel"))) {
        problem->current_fuel = parse_double(&p);
    }
    if ((p = find_json_key(json, "consumption_mpg"))) {
        problem->base_consumption_mpg = parse_double(&p);
    }
    if ((p = find_json_key(json, "minimum_fuel"))) {
        problem->minimum_fuel = parse_double(&p);
    }
    if ((p = find_json_key(json, "minimum_fuel_at_end"))) {
        problem->minimum_fuel_at_end = parse_double(&p);
    } else {
        problem->minimum_fuel_at_end = problem->minimum_fuel;
    }
    if ((p = find_json_key(json, "min_purchase"))) {
        problem->min_purchase = parse_double(&p);
    }
    if ((p = find_json_key(json, "stop_cost"))) {
        problem->stop_cost = parse_double(&p);
    }

    /* Parse segments array (optional - for variable consumption) */
    p = find_json_key(json, "segments");
    if (p && *p == '[') {
        FWRouteSegment *segments = NULL;
        int num_segments = 0;
        if (parse_segments(p, &segments, &num_segments) == 0 && num_segments > 0) {
            problem->segments = segments;
            problem->num_segments = num_segments;
        }
    }

    /* Parse stations array */
    p = find_json_key(json, "stations");
    if (!p || *p != '[') {
        return -1;  /* stations required */
    }

    /* Count stations */
    int count = 0;
    const char *scan = p + 1;
    int depth = 0;
    while (*scan) {
        if (*scan == '{') {
            if (depth == 0) count++;
            depth++;
        } else if (*scan == '}') {
            depth--;
        } else if (*scan == ']' && depth == 0) {
            break;
        }
        scan++;
    }

    if (count == 0) {
        return -1;
    }

    /* Allocate stations */
    problem->stations = calloc((size_t)count, sizeof(FWSnappedStation));
    if (!problem->stations) return -1;
    problem->num_stations = count;

    /* Parse each station */
    p++;  /* skip '[' */
    for (int i = 0; i < count; i++) {
        p = skip_ws(p);
        if (*p != '{') break;

        const char *obj_start = p;
        const char *obj_end = strchr(p, '}');
        if (!obj_end) break;

        /* Parse station fields */
        const char *sp;
        if ((sp = find_json_key(obj_start, "station_id"))) {
            problem->stations[i].station_id = parse_int(&sp);
        } else if ((sp = find_json_key(obj_start, "id"))) {
            problem->stations[i].station_id = parse_int(&sp);
        }
        if ((sp = find_json_key(obj_start, "distance_from_start"))) {
            problem->stations[i].distance_from_start = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "distance"))) {
            problem->stations[i].distance_from_start = parse_double(&sp);
        }
        if ((sp = find_json_key(obj_start, "price_per_gallon"))) {
            problem->stations[i].price_per_gallon = parse_double(&sp);
        } else if ((sp = find_json_key(obj_start, "price"))) {
            problem->stations[i].price_per_gallon = parse_double(&sp);
        }

        p = obj_end + 1;
        p = skip_ws(p);
        if (*p == ',') p++;
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
static char *process_solve(const char *body, int *status_code) {
    *status_code = 200;

    /* Parse request body */
    FWRefuelProblem problem;
    if (parse_solve_request(body, &problem) != 0) {
        *status_code = 400;
        return strdup("{\"error\": \"Invalid request format\"}\n");
    }

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
                solution.purchases[i] * problem.stations[i].price_per_gallon);
            if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
        }
    }

    snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    fw_free_solution(&solution);
    free_problem(&problem);
    return response;
}

/* Process a filter request - returns malloc'd response string */
static char *process_filter(const char *body, int *status_code) {
    *status_code = 200;

    /* Parse stations array */
    const char *stations_json = find_json_key(body, "stations");
    if (!stations_json) {
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'stations' array\"}\n");
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        *status_code = 400;
        return strdup("{\"error\": \"Invalid stations format\"}\n");
    }

    /* Parse route polyline */
    const char *route_json = find_json_key(body, "route");
    if (!route_json) {
        free(stations);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'route' array\"}\n");
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid route format\"}\n");
    }

    /* Parse max_distance (default 5 miles) */
    double max_distance = 5.0;
    const char *p;
    if ((p = find_json_key(body, "max_distance"))) {
        max_distance = parse_double(&p);
    }

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
            "\"price_per_gallon\": %.3f, "
            "\"snap_point\": [%.6f, %.6f]"
            "}",
            filtered[i].station_id,
            filtered[i].distance_from_start,
            filtered[i].perpendicular_distance,
            filtered[i].price_per_gallon,
            filtered[i].snap_point.lat,
            filtered[i].snap_point.lon);
        if (n > 0 && (size_t)n < buf_size - pos) pos += (size_t)n;
    }

    snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    fw_free_snapped_stations(filtered);
    return response;
}

/* Process an optimize request - returns malloc'd response string */
static char *process_optimize(const char *body, int *status_code) {
    *status_code = 200;

    /* Parse stations array */
    const char *stations_json = find_json_key(body, "stations");
    if (!stations_json) {
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'stations' array\"}\n");
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        *status_code = 400;
        return strdup("{\"error\": \"Invalid stations format\"}\n");
    }

    /* Parse route polyline */
    const char *route_json = find_json_key(body, "route");
    if (!route_json) {
        free(stations);
        *status_code = 400;
        return strdup("{\"error\": \"Missing 'route' array\"}\n");
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        *status_code = 400;
        return strdup("{\"error\": \"Invalid route format\"}\n");
    }

    /* Parse config */
    const char *p;
    double tank_capacity = 100.0;
    double current_fuel = 50.0;
    double consumption_mpg = 6.5;
    double min_fuel = 25.0;
    double max_distance = 5.0;
    double min_purchase = 0.0;
    double stop_cost = 0.0;

    if ((p = find_json_key(body, "tank_capacity"))) {
        tank_capacity = parse_double(&p);
    }
    if ((p = find_json_key(body, "current_fuel"))) {
        current_fuel = parse_double(&p);
    }
    if ((p = find_json_key(body, "consumption_mpg"))) {
        consumption_mpg = parse_double(&p);
    }
    if ((p = find_json_key(body, "minimum_fuel"))) {
        min_fuel = parse_double(&p);
    }
    if ((p = find_json_key(body, "max_distance"))) {
        max_distance = parse_double(&p);
    }
    if ((p = find_json_key(body, "min_purchase"))) {
        min_purchase = parse_double(&p);
    }
    if ((p = find_json_key(body, "stop_cost"))) {
        stop_cost = parse_double(&p);
    }
    double remaining_fuel_value = 0.0;
    if ((p = find_json_key(body, "remaining_fuel_value"))) {
        remaining_fuel_value = parse_double(&p);
    }

    /* Parse segments (optional) */
    FWRouteSegment *segments = NULL;
    int num_segments = 0;
    p = find_json_key(body, "segments");
    if (p && *p == '[') {
        parse_segments(p, &segments, &num_segments);
    }

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
    problem.base_consumption_mpg = consumption_mpg;
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
                solution.purchases[i] * filtered[i].price_per_gallon);
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
            /* Ensure null-terminated body */
            char *body_copy = malloc(req->body_len + 1);
            if (body_copy) {
                memcpy(body_copy, req->body, req->body_len);
                body_copy[req->body_len] = '\0';
                response_body = process_solve(body_copy, &status_code);
                free(body_copy);
            } else {
                status_code = 500;
                response_body = strdup("{\"error\": \"Memory allocation failed\"}\n");
            }
        }
    }
    else if (strcmp(req->path, "/api/v1/filter") == 0) {
        if (!req->body || req->body_len == 0) {
            status_code = 400;
            response_body = strdup("{\"error\": \"Missing request body\"}\n");
        } else {
            char *body_copy = malloc(req->body_len + 1);
            if (body_copy) {
                memcpy(body_copy, req->body, req->body_len);
                body_copy[req->body_len] = '\0';
                response_body = process_filter(body_copy, &status_code);
                free(body_copy);
            } else {
                status_code = 500;
                response_body = strdup("{\"error\": \"Memory allocation failed\"}\n");
            }
        }
    }
    else if (strcmp(req->path, "/api/v1/optimize") == 0) {
        if (!req->body || req->body_len == 0) {
            status_code = 400;
            response_body = strdup("{\"error\": \"Missing request body\"}\n");
        } else {
            char *body_copy = malloc(req->body_len + 1);
            if (body_copy) {
                memcpy(body_copy, req->body, req->body_len);
                body_copy[req->body_len] = '\0';
                response_body = process_optimize(body_copy, &status_code);
                free(body_copy);
            } else {
                status_code = 500;
                response_body = strdup("{\"error\": \"Memory allocation failed\"}\n");
            }
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
