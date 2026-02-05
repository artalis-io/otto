/*
 * FuelWise REST API Server
 *
 * A lightweight HTTP API for fuel optimization using mongoose.
 * Features rate limiting, work queue for CPU-intensive operations,
 * and configurable through CLI args and environment variables.
 *
 * Endpoints:
 *   GET  /api/v1/health         - Health check (bypasses queue)
 *   GET  /api/v1/stats          - Server statistics (bypasses queue)
 *   POST /api/v1/filter         - Filter stations to route
 *   POST /api/v1/solve          - Solve refueling problem
 *   POST /api/v1/optimize       - Full optimization pipeline
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <stdint.h>
#include "mongoose.h"
#include "fuelwise.h"
#include "shared.h"  /* For sh_ratelimit, sh_workqueue, sh_args */
#include "sh_httpserver.h"  /* For sh_mg_set_write_timeout */
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Global state */
static volatile sig_atomic_t s_signo = 0;

/* Rate limiter instance */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance */
static ShWorkQueue *s_work_queue = NULL;

/* Server configuration (from sh_args) */
static ShServerConfig s_config;

/* CORS configuration */
static ShCorsConfig s_cors;

/* Work item types for the queue */
typedef enum {
    WORK_TYPE_SOLVE,
    WORK_TYPE_FILTER,
    WORK_TYPE_OPTIMIZE
} WorkType;

/* Work item for CPU-intensive operations */
typedef struct {
    WorkType type;
    char *request_body;      /* Copy of request body (owned) */
    size_t request_len;

    /* Response buffer (set by worker) */
    char *response_data;
    size_t response_size;
    int status_code;

    /* Completion signaling */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int completed;
    volatile int cancelled;  /* Set by HTTP handler on timeout */
} SolveWorkItem;

/* Worker threads */
static pthread_t *s_workers = NULL;
static int s_num_workers = 0;
static volatile int s_shutdown = 0;

/* Signal handler */
static void signal_handler(int signo) {
    s_signo = signo;
}

/* Trace ID header getter for HTTP requests */
static const char *trace_header_getter(const char *name, void *ctx) {
    struct mg_http_message *hm = (struct mg_http_message *)ctx;
    struct mg_str *hdr = mg_http_get_header(hm, name);
    if (hdr && hdr->len > 0) {
        static __thread char hdr_buf[128];
        size_t len = hdr->len < sizeof(hdr_buf) - 1 ? hdr->len : sizeof(hdr_buf) - 1;
        memcpy(hdr_buf, hdr->buf, len);
        hdr_buf[len] = '\0';
        return hdr_buf;
    }
    return NULL;
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

/* Build error response - uses shared helper */
static void send_error(struct mg_connection *c, int status, const char *message) {
    sh_mg_reply_error(c, status, &s_cors, NULL, message);
}

/* Build success response with JSON body - uses shared helper */
static void send_json(struct mg_connection *c, const char *json) {
    sh_mg_reply_json(c, 200, &s_cors, NULL, json);
}

/* Build response with custom status code and JSON body - uses shared helper */
static void send_json_status(struct mg_connection *c, int status, const char *json) {
    sh_mg_reply_json(c, status, &s_cors, NULL, json);
}

/* ============================================================================
 * Core Processing Functions (called by workers)
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

/* ============================================================================
 * Worker Thread
 * ============================================================================ */

static void *worker_thread_fn(void *arg) {
    (void)arg;

    while (!s_shutdown) {
        ShWorkItem *item = sh_workqueue_pop_timeout(s_work_queue, 100);
        if (!item) continue;

        /* Check if item expired */
        if (sh_workqueue_item_expired(s_work_queue, item)) {
            sh_workqueue_item_free(item);
            continue;
        }

        /* Get the work item */
        SolveWorkItem *work = (SolveWorkItem *)item->user_ctx;
        if (!work) {
            sh_workqueue_item_free(item);
            continue;
        }

        /* Check if HTTP handler already timed out and cancelled */
        if (work->cancelled) {
            /* Clean up the cancelled work item */
            free(work->request_body);
            pthread_mutex_destroy(&work->mutex);
            pthread_cond_destroy(&work->cond);
            free(work);
            sh_workqueue_item_free(item);
            continue;
        }

        /* Process based on type */
        int status_code = 200;
        char *response = NULL;

        switch (work->type) {
            case WORK_TYPE_SOLVE:
                response = process_solve(work->request_body, &status_code);
                break;
            case WORK_TYPE_FILTER:
                response = process_filter(work->request_body, &status_code);
                break;
            case WORK_TYPE_OPTIMIZE:
                response = process_optimize(work->request_body, &status_code);
                break;
        }

        /* Store result */
        pthread_mutex_lock(&work->mutex);
        work->response_data = response;
        work->response_size = response ? strlen(response) : 0;
        work->status_code = status_code;
        work->completed = 1;
        pthread_cond_signal(&work->cond);
        pthread_mutex_unlock(&work->mutex);

        sh_workqueue_item_free(item);
    }

    return NULL;
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

/* GET /api/v1/health - bypasses work queue, uses shared helper */
static void handle_health(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    sh_mg_handle_health(c, &s_cors, NULL, "fuelwise-api", fw_version());
}

/* GET /api/v1/stats - bypasses work queue */
static void handle_stats(struct mg_connection *c, struct mg_http_message *hm) {
    (void)hm;
    char response[2048];
    size_t pos = 0;
    int n;

    n = snprintf(response + pos, sizeof(response) - pos,
        "{\n"
        "  \"service\": \"fuelwise-api\",\n"
        "  \"version\": \"%s\",\n",
        fw_version());
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    /* Work queue stats */
    if (s_work_queue) {
        ShWorkQueueStats wq_stats;
        sh_workqueue_stats(s_work_queue, &wq_stats);
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"work_queue\": {\n"
            "    \"enabled\": true,\n"
            "    \"depth\": %zu,\n"
            "    \"capacity\": %zu,\n"
            "    \"pushed\": %llu,\n"
            "    \"popped\": %llu,\n"
            "    \"dropped\": %llu,\n"
            "    \"expired\": %llu,\n"
            "    \"timeout_sec\": %.1f\n"
            "  },\n",
            wq_stats.current_depth,
            wq_stats.max_capacity,
            (unsigned long long)wq_stats.total_pushed,
            (unsigned long long)wq_stats.total_popped,
            (unsigned long long)wq_stats.total_dropped,
            (unsigned long long)wq_stats.total_expired,
            wq_stats.timeout_sec);
    } else {
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"work_queue\": {\n"
            "    \"enabled\": false\n"
            "  },\n");
    }
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    /* Rate limit stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"rate_limit\": {\n"
            "    \"enabled\": true,\n"
            "    \"rps\": %.1f,\n"
            "    \"burst\": %.1f,\n"
            "    \"allowed\": %llu,\n"
            "    \"denied\": %llu,\n"
            "    \"active_entries\": %zu,\n"
            "    \"evictions\": %zu\n"
            "  }\n",
            s_config.rate_limit_rps,
            s_config.rate_limit_burst,
            (unsigned long long)rl_stats.requests_allowed,
            (unsigned long long)rl_stats.requests_denied,
            rl_stats.active_entries,
            rl_stats.evictions);
    } else {
        n = snprintf(response + pos, sizeof(response) - pos,
            "  \"rate_limit\": {\n"
            "    \"enabled\": false\n"
            "  }\n");
    }
    if (n > 0 && (size_t)n < sizeof(response) - pos) pos += (size_t)n;

    snprintf(response + pos, sizeof(response) - pos, "}\n");
    send_json(c, response);
}

/* Generic handler that uses work queue */
static void handle_via_queue(struct mg_connection *c, struct mg_http_message *hm, WorkType type) {
    if (!s_work_queue) {
        /* Work queue disabled - process synchronously */
        int status_code = 200;
        char *response = NULL;

        /* Copy body to null-terminated string */
        char *body = malloc(hm->body.len + 1);
        if (!body) {
            send_error(c, 500, "Memory allocation failed");
            return;
        }
        memcpy(body, hm->body.buf, hm->body.len);
        body[hm->body.len] = '\0';

        switch (type) {
            case WORK_TYPE_SOLVE:
                response = process_solve(body, &status_code);
                break;
            case WORK_TYPE_FILTER:
                response = process_filter(body, &status_code);
                break;
            case WORK_TYPE_OPTIMIZE:
                response = process_optimize(body, &status_code);
                break;
        }

        free(body);

        if (response) {
            send_json_status(c, status_code, response);
            free(response);
        } else {
            send_error(c, 500, "Processing failed");
        }
        return;
    }

    /* Create work item */
    SolveWorkItem *work = calloc(1, sizeof(SolveWorkItem));
    if (!work) {
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    work->type = type;
    work->request_body = malloc(hm->body.len + 1);
    if (!work->request_body) {
        free(work);
        send_error(c, 500, "Memory allocation failed");
        return;
    }
    memcpy(work->request_body, hm->body.buf, hm->body.len);
    work->request_body[hm->body.len] = '\0';
    work->request_len = hm->body.len;
    work->completed = 0;

    pthread_mutex_init(&work->mutex, NULL);
    pthread_cond_init(&work->cond, NULL);

    /* Push to work queue */
    ShWorkItem item = {
        .data = NULL,  /* We manage our own data */
        .data_len = 0,
        .user_ctx = work
    };

    double pressure = 0.0;
    if (!sh_workqueue_try_push(s_work_queue, &item, &pressure)) {
        /* Queue full - backpressure */
        pthread_mutex_destroy(&work->mutex);
        pthread_cond_destroy(&work->cond);
        free(work->request_body);
        free(work);
        send_error(c, 503, "Service unavailable - queue full");
        return;
    }

    /* Wait for completion with timeout */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (int)s_config.work_queue_timeout;

    pthread_mutex_lock(&work->mutex);
    while (!work->completed) {
        int rc = pthread_cond_timedwait(&work->cond, &work->mutex, &ts);
        if (rc != 0) {
            /* Timeout - mark item as cancelled so worker can skip if not started */
            work->cancelled = 1;
            pthread_mutex_unlock(&work->mutex);
            send_error(c, 504, "Gateway timeout");
            /* Note: work will be cleaned up when worker processes it */
            return;
        }
    }
    pthread_mutex_unlock(&work->mutex);

    /* Send response */
    if (work->response_data) {
        send_json_status(c, work->status_code, work->response_data);
        free(work->response_data);
    } else {
        send_error(c, 500, "Processing failed");
    }

    /* Cleanup */
    pthread_mutex_destroy(&work->mutex);
    pthread_cond_destroy(&work->cond);
    free(work->request_body);
    free(work);
}

/* POST /api/v1/solve */
static void handle_solve(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, WORK_TYPE_SOLVE);
}

/* POST /api/v1/filter */
static void handle_filter(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, WORK_TYPE_FILTER);
}

/* POST /api/v1/optimize */
static void handle_optimize(struct mg_connection *c, struct mg_http_message *hm) {
    handle_via_queue(c, hm, WORK_TYPE_OPTIMIZE);
}

/* GET /metrics - Prometheus metrics endpoint, uses shared helper */
static void handle_metrics(struct mg_connection *c) {
    sh_mg_handle_metrics(c);
}

/* OPTIONS handler for CORS preflight */
static void handle_options(struct mg_connection *c) {
    char cors_headers[512];
    sh_cors_preflight_headers(&s_cors, NULL, cors_headers, sizeof(cors_headers));
    mg_http_reply(c, 204, cors_headers, "");
}

/* ============================================================================
 * Main Event Handler
 * ============================================================================ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    /* Set socket write timeout on new connections to protect against slow clients */
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);  /* 5 second write timeout */
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Start request timing */
        ShMetricsTimer req_timer = sh_metrics_timer_start();

        /* Extract or generate trace ID */
        sh_trace_from_headers(trace_header_getter, hm);

        /* Handle CORS preflight - no rate limiting */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            handle_options(c);
            sh_trace_clear();
            return;
        }

        /* Health, stats, and metrics bypass rate limiting and work queue */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "health", "service", "fuelwise", NULL);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                "endpoint", "health", "service", "fuelwise", NULL);
            sh_trace_clear();
            return;
        }
        if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "stats", "service", "fuelwise", NULL);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                "endpoint", "stats", "service", "fuelwise", NULL);
            sh_trace_clear();
            return;
        }
        if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
            handle_metrics(c);
            sh_trace_clear();
            return;
        }

        /* Check rate limit for all other endpoints - uses shared helper */
        if (!sh_mg_check_rate_limit(c, s_rate_limiter, &s_cors, NULL)) {
            sh_metrics_counter_inc("http_requests_total", 1,
                "endpoint", "rate_limited", "status", "429", NULL);
            sh_trace_clear();
            return;
        }

        /* Route requests */
        const char *endpoint = "unknown";
        if (mg_match(hm->uri, mg_str("/api/v1/solve"), NULL)) {
            endpoint = "solve";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_solve(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/optimize"), NULL)) {
            endpoint = "optimize";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_optimize(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else if (mg_match(hm->uri, mg_str("/api/v1/filter"), NULL)) {
            endpoint = "filter";
            if (mg_match(hm->method, mg_str("POST"), NULL)) {
                handle_filter(c, hm);
            } else {
                send_error(c, 405, "Method not allowed");
            }
        } else {
            endpoint = "not_found";
            send_error(c, 404, "Not found");
        }

        /* Record metrics */
        sh_metrics_counter_inc("http_requests_total", 1,
            "endpoint", endpoint, "service", "fuelwise", NULL);
        sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
            "endpoint", endpoint, "service", "fuelwise", NULL);

        /* Clear trace context */
        sh_trace_clear();
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    sh_args_usage(prog,
        "<no-data-file>\n\n"
        "FuelWise API server for fuel optimization.\n\n"
        "Example:\n"
        "  %s -p 8080                    # Start on port 8080\n"
        "  %s --rate-limit-off           # Disable rate limiting\n"
        "  %s --queue-off                # Disable work queue\n"
    );
}

int main(int argc, char *argv[]) {
    /* Initialize config with defaults */
    sh_args_init(&s_config);
    sh_cors_init(&s_cors);

    /* FuelWise-specific defaults */
    s_config.port = 8080;
    s_config.rate_limit_rps = 10.0;
    s_config.rate_limit_burst = 50.0;
    s_config.work_queue_depth = 100;
    s_config.work_queue_timeout = 10.0;
    s_config.worker_threads = 4;

    /* Load from environment first */
    sh_args_load_env(&s_config, SH_API_FUELWISE);

    /* Parse command line (overrides env) */
    int first_arg = sh_args_parse(&s_config, argc, argv);
    if (first_arg < 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* Check for help flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize logging */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "fuelwise";
    log_cfg.version = fw_version();
    sh_log_init(&log_cfg);

    /* Initialize metrics */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "fuelwise";
    sh_metrics_init(&metrics_cfg);

    SH_LOG_INFO("Starting fuelwise API server", "version", fw_version(), NULL);

    /* Initialize rate limiter */
    if (s_config.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(
            s_config.rate_limit_rps,
            s_config.rate_limit_burst,
            s_config.rate_limit_buckets > 0 ? s_config.rate_limit_buckets : 4096
        );
        if (!s_rate_limiter) {
            fprintf(stderr, "Error: Failed to create rate limiter\n");
            return 1;
        }
    }

    /* Initialize work queue and workers */
    if (s_config.work_queue_enabled) {
        s_work_queue = sh_workqueue_create(
            s_config.work_queue_depth,
            s_config.work_queue_timeout
        );
        if (!s_work_queue) {
            fprintf(stderr, "Error: Failed to create work queue\n");
            sh_ratelimit_free(s_rate_limiter);
            return 1;
        }

        /* Start worker threads */
        s_num_workers = s_config.worker_threads > 0 ? s_config.worker_threads : 4;
        s_workers = calloc(s_num_workers, sizeof(pthread_t));
        if (!s_workers) {
            fprintf(stderr, "Error: Failed to allocate worker threads\n");
            sh_workqueue_free(s_work_queue);
            sh_ratelimit_free(s_rate_limiter);
            return 1;
        }

        for (int i = 0; i < s_num_workers; i++) {
            if (pthread_create(&s_workers[i], NULL, worker_thread_fn, NULL) != 0) {
                fprintf(stderr, "Error: Failed to create worker thread %d\n", i);
                /* Continue with fewer workers */
                s_num_workers = i;
                break;
            }
        }
    }

    /* Initialize mongoose */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* Build listen address */
    char listen_addr[128];
    snprintf(listen_addr, sizeof(listen_addr), "http://%s:%d",
        s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);

    /* Start listening */
    struct mg_connection *c = mg_http_listen(&mgr, listen_addr, ev_handler, NULL);
    if (c == NULL) {
        fprintf(stderr, "Error: Cannot listen on %s\n", listen_addr);
        goto cleanup;
    }

    /* Print startup message */
    printf("FuelWise API Server v%s\n", fw_version());
    printf("Listening on http://%s:%d\n",
        s_config.host[0] ? s_config.host : "0.0.0.0", s_config.port);
    printf("\n");
    printf("Configuration:\n");
    printf("  Rate limiting: %s", s_config.rate_limit_enabled ? "enabled" : "disabled");
    if (s_config.rate_limit_enabled) {
        printf(" (%.1f RPS, burst %.0f)", s_config.rate_limit_rps, s_config.rate_limit_burst);
    }
    printf("\n");
    printf("  Work queue: %s", s_config.work_queue_enabled ? "enabled" : "disabled");
    if (s_config.work_queue_enabled) {
        printf(" (depth %zu, timeout %.1fs, %d workers)",
            s_config.work_queue_depth, s_config.work_queue_timeout, s_num_workers);
    }
    printf("\n");
    printf("\n");
    printf("Endpoints:\n");
    printf("  GET  /api/v1/health    - Health check\n");
    printf("  GET  /api/v1/stats     - Server statistics\n");
    printf("  POST /api/v1/solve     - Solve refueling problem\n");
    printf("  POST /api/v1/filter    - Filter stations to route\n");
    printf("  POST /api/v1/optimize  - Full optimization pipeline\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 1000);
    }

    printf("\nShutting down...\n");

cleanup:
    /* Shutdown workers */
    s_shutdown = 1;
    if (s_work_queue) {
        sh_workqueue_shutdown(s_work_queue);
    }

    /* Join worker threads */
    for (int i = 0; i < s_num_workers; i++) {
        pthread_join(s_workers[i], NULL);
    }
    free(s_workers);

    /* Cleanup */
    mg_mgr_free(&mgr);
    sh_workqueue_free(s_work_queue);
    sh_ratelimit_free(s_rate_limiter);

    return 0;
}
