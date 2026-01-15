/*
 * FuelWise REST API Server
 *
 * A lightweight HTTP API for fuel optimization using mongoose.
 *
 * Endpoints:
 *   GET  /api/v1/health         - Health check
 *   POST /api/v1/filter         - Filter stations to route
 *   POST /api/v1/solve          - Solve refueling problem
 *   POST /api/v1/optimize       - Full optimization pipeline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "mongoose.h"
#include "fuelwise.h"

/* Default configuration */
#define DEFAULT_PORT "8080"
#define MAX_REQUEST_SIZE (10 * 1024 * 1024)  /* 10 MB */

static int s_signo = 0;
static void signal_handler(int signo) {
    s_signo = signo;
}

/* ============================================================================
 * JSON Parsing Helpers (Simple implementation)
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

/* Find a key in a JSON object and return pointer to value */
static const char* find_json_key(const char *json, const char *key) {
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
    polyline->points = calloc(count, sizeof(FWCoord));
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

/* Parse a JSON array of route segments: [{"start_distance": 0, "weight": 40000, "mpg": 6.5}, ...] */
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
    *segments = calloc(seg_count, sizeof(FWRouteSegment));
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
    *stations = calloc(station_count, sizeof(FWStation));
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

/* ============================================================================
 * Request Parsing
 * ============================================================================ */

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
    problem->stations = calloc(count, sizeof(FWSnappedStation));
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
 * Response Building
 * ============================================================================ */

/* Build error response */
static void send_error(struct mg_connection *c, int status, const char *message) {
    mg_http_reply(c, status, "Content-Type: application/json\r\n",
        "{\"error\": \"%s\"}\n", message);
}

/* Build success response with JSON body */
static void send_json(struct mg_connection *c, const char *json) {
    mg_http_reply(c, 200,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "%s", json);
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

/* GET /api/v1/health */
static void handle_health(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    char response[512];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"version\": \"%s\",\n"
        "  \"service\": \"fuelwise-api\"\n"
        "}\n",
        fw_version());
    send_json(c, response);
}

/* POST /api/v1/solve */
static void handle_solve(struct mg_connection *c, struct mg_http_message *hm) {
    /* Parse request body */
    FWRefuelProblem problem;
    if (parse_solve_request(hm->body.buf, &problem) != 0) {
        send_error(c, 400, "Invalid request format");
        return;
    }

    /* Validate problem */
    char error_msg[256];
    if (!fw_validate_problem(&problem, error_msg, sizeof(error_msg))) {
        send_error(c, 400, error_msg);
        free_problem(&problem);
        return;
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
        send_error(c, 422, fw_status_string(solution.status));
        fw_free_solution(&solution);
        free_problem(&problem);
        return;
    }

    /* Build response */
    int buf_size = 2048 + problem.num_stations * 128;
    char *response = malloc(buf_size);
    if (!response) {
        send_error(c, 500, "Memory allocation failed");
        fw_free_solution(&solution);
        free_problem(&problem);
        return;
    }

    int pos = 0;
    pos += snprintf(response + pos, buf_size - pos,
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

    int first = 1;
    for (int i = 0; i < problem.num_stations; i++) {
        if (solution.purchases[i] > 0.001) {
            if (!first) pos += snprintf(response + pos, buf_size - pos, ",");
            first = 0;
            pos += snprintf(response + pos, buf_size - pos,
                "\n    {"
                "\"station_id\": %d, "
                "\"gallons\": %.2f, "
                "\"cost\": %.2f"
                "}",
                problem.stations[i].station_id,
                solution.purchases[i],
                solution.purchases[i] * problem.stations[i].price_per_gallon);
        }
    }

    pos += snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    send_json(c, response);

    free(response);
    fw_free_solution(&solution);
    free_problem(&problem);
}

/* POST /api/v1/filter */
static void handle_filter(struct mg_connection *c, struct mg_http_message *hm) {
    /* Parse stations array */
    const char *stations_json = find_json_key(hm->body.buf, "stations");
    if (!stations_json) {
        send_error(c, 400, "Missing 'stations' array");
        return;
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        send_error(c, 400, "Invalid stations format");
        return;
    }

    /* Parse route polyline */
    const char *route_json = find_json_key(hm->body.buf, "route");
    if (!route_json) {
        free(stations);
        send_error(c, 400, "Missing 'route' array");
        return;
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        send_error(c, 400, "Invalid route format");
        return;
    }

    /* Parse max_distance (default 5 miles) */
    double max_distance = 5.0;
    const char *p;
    if ((p = find_json_key(hm->body.buf, "max_distance"))) {
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
        send_error(c, 500, "Filter operation failed");
        return;
    }

    /* Build response */
    int buf_size = 1024 + filtered_count * 256;
    char *response = malloc(buf_size);
    if (!response) {
        fw_free_snapped_stations(filtered);
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    int pos = 0;
    pos += snprintf(response + pos, buf_size - pos,
        "{\n"
        "  \"count\": %d,\n"
        "  \"stations\": [",
        filtered_count);

    for (int i = 0; i < filtered_count; i++) {
        if (i > 0) pos += snprintf(response + pos, buf_size - pos, ",");
        pos += snprintf(response + pos, buf_size - pos,
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
    }

    pos += snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    send_json(c, response);

    free(response);
    fw_free_snapped_stations(filtered);
}

/* POST /api/v1/optimize - Full optimization pipeline */
static void handle_optimize(struct mg_connection *c, struct mg_http_message *hm) {
    /* Parse stations array */
    const char *stations_json = find_json_key(hm->body.buf, "stations");
    if (!stations_json) {
        send_error(c, 400, "Missing 'stations' array");
        return;
    }

    FWStation *stations = NULL;
    int num_stations = 0;
    if (parse_stations_geo(stations_json, &stations, &num_stations) != 0) {
        send_error(c, 400, "Invalid stations format");
        return;
    }

    /* Parse route polyline */
    const char *route_json = find_json_key(hm->body.buf, "route");
    if (!route_json) {
        free(stations);
        send_error(c, 400, "Missing 'route' array");
        return;
    }

    FWPolyline route;
    if (parse_polyline(route_json, &route) != 0) {
        free(stations);
        send_error(c, 400, "Invalid route format");
        return;
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

    if ((p = find_json_key(hm->body.buf, "tank_capacity"))) {
        tank_capacity = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "current_fuel"))) {
        current_fuel = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "consumption_mpg"))) {
        consumption_mpg = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "minimum_fuel"))) {
        min_fuel = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "max_distance"))) {
        max_distance = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "min_purchase"))) {
        min_purchase = parse_double(&p);
    }
    if ((p = find_json_key(hm->body.buf, "stop_cost"))) {
        stop_cost = parse_double(&p);
    }

    /* Parse segments (optional - for variable consumption) */
    FWRouteSegment *segments = NULL;
    int num_segments = 0;
    p = find_json_key(hm->body.buf, "segments");
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
        send_error(c, 422, filtered_count == 0 ?
            "No stations found within distance of route" : "Filter failed");
        return;
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
    problem.num_stations = filtered_count;
    problem.stations = filtered;

    /* Add segments if provided */
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
        send_error(c, 400, error_msg);
        return;
    }

    /* Solve - use MILP if we have min_purchase or stop_cost */
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
        send_error(c, 422, fw_status_string(solution.status));
        fw_free_solution(&solution);
        return;
    }

    /* Build response */
    int buf_size = 2048 + filtered_count * 256;
    char *response = malloc(buf_size);
    if (!response) {
        fw_free_snapped_stations(filtered);
        fw_free_solution(&solution);
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    int pos = 0;
    pos += snprintf(response + pos, buf_size - pos,
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

    int first = 1;
    for (int i = 0; i < filtered_count; i++) {
        if (solution.purchases[i] > 0.001) {
            if (!first) pos += snprintf(response + pos, buf_size - pos, ",");
            first = 0;
            pos += snprintf(response + pos, buf_size - pos,
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
        }
    }

    pos += snprintf(response + pos, buf_size - pos, "\n  ]\n}\n");

    send_json(c, response);

    free(response);
    fw_free_snapped_stations(filtered);
    fw_free_solution(&solution);
    free(segments);
}

/* OPTIONS handler for CORS preflight */
static void handle_options(struct mg_connection *c) {
    mg_http_reply(c, 204,
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Access-Control-Max-Age: 86400\r\n",
        "");
}

/* ============================================================================
 * Main Event Handler
 * ============================================================================ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Handle CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            handle_options(c);
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c, hm);
        } else if (mg_match(hm->uri, mg_str("/api/v1/solve"), NULL)) {
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_solve(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/optimize"), NULL)) {
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_optimize(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/filter"), NULL)) {
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_filter(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else {
            send_error(c, 404, "Not found");
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    const char *port = DEFAULT_PORT;

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("FuelWise API Server\n");
            printf("Usage: %s [-p port]\n", argv[0]);
            printf("  -p port    Port to listen on (default: %s)\n", DEFAULT_PORT);
            return 0;
        }
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize mongoose */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Build listen address */
    char listen_addr[64];
    snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%s", port);

    /* Start listening */
    struct mg_connection *c = mg_http_listen(&mgr, listen_addr, ev_handler, NULL);
    if (c == NULL) {
        fprintf(stderr, "Error: Cannot listen on %s\n", listen_addr);
        return 1;
    }

    printf("FuelWise API Server v%s\n", fw_version());
    printf("Listening on http://0.0.0.0:%s\n", port);
    printf("Endpoints:\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  POST /api/v1/solve     - Solve refueling problem (pre-snapped stations)\n");
    printf("  POST /api/v1/filter    - Filter stations to route polyline\n");
    printf("  POST /api/v1/optimize  - Full optimization (filter + solve)\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 1000);
    }

    printf("\nShutting down...\n");
    mg_mgr_free(&mgr);

    return 0;
}
