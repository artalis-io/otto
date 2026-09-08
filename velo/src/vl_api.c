/*
 * Velo API Handler - Transport-agnostic implementation
 *
 * This file contains the core request handling logic that can be used by
 * any transport layer (HTTP, WASM, etc.).
 */

#include "vl_api.h"
#include "sh_polyline.h"
#include "sh_json.h"
#include "sh_arena.h"
#include "sh_geo.h"
#include "sh_api.h"
#include "vl_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ============================================================================
 * JSON Response Helpers
 * ============================================================================ */

/* Build an error response using ShJsonWriter */
static char *make_error_response(const char *message, size_t *out_len) {
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_kv_string(&jw, "error", message ? message : "Unknown error");
    sh_json_write_object_end(&jw);

    if (sh_json_writer_error(&jw)) {
        sh_json_buf_free(&jb);
        char *fallback = strdup("{\"error\":\"JSON write error\"}");
        if (out_len) *out_len = fallback ? strlen(fallback) : 0;
        return fallback;
    }

    char *result = sh_json_buf_take(&jb);
    if (out_len) *out_len = result ? strlen(result) : 0;
    return result;
}

/* ============================================================================
 * API Context
 * ============================================================================ */

struct VLAPIContext {
    VLGraph *graph;           /* NOT owned */
    VLLandmarks *landmarks;   /* NOT owned, may be NULL */
    char graph_path[512];
    char name[128];
    int landmark_count;
};

void vl_api_config_init(VLAPIConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->graph_path = "";
    config->name = "velo-route-server";
    config->landmark_count = 0;
}

VLAPIContext *vl_api_create(VLGraph *graph, VLLandmarks *landmarks,
                            const VLAPIConfig *config) {
    if (!graph) return NULL;

    VLAPIContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->graph = graph;
    ctx->landmarks = landmarks;

    if (config) {
        if (config->graph_path) {
            strncpy(ctx->graph_path, config->graph_path, sizeof(ctx->graph_path) - 1);
            ctx->graph_path[sizeof(ctx->graph_path) - 1] = '\0';
        }
        if (config->name) {
            strncpy(ctx->name, config->name, sizeof(ctx->name) - 1);
            ctx->name[sizeof(ctx->name) - 1] = '\0';
        }
        ctx->landmark_count = config->landmark_count;
    } else {
        strncpy(ctx->name, "velo-route-server", sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    }

    return ctx;
}

void vl_api_free(VLAPIContext *ctx) {
    if (!ctx) return;
    /* Do NOT free graph or landmarks - we don't own them */
    free(ctx);
}

VLGraph *vl_api_get_graph(VLAPIContext *ctx) {
    return ctx ? ctx->graph : NULL;
}

VLLandmarks *vl_api_get_landmarks(VLAPIContext *ctx) {
    return ctx ? ctx->landmarks : NULL;
}

/* ============================================================================
 * Query String Parsing Utilities
 * ============================================================================ */

/* Extract a query parameter value by name */
static int get_query_param(const char *query, const char *name,
                           char *value, size_t value_size) {
    if (!query || !name || !value || value_size == 0) return -1;

    value[0] = '\0';
    size_t name_len = strlen(name);
    const char *p = query;

    while (*p) {
        /* Check if this is the parameter we're looking for */
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *val_start = p + name_len + 1;
            const char *val_end = val_start;

            /* Find end of value (& or end of string) */
            while (*val_end && *val_end != '&') val_end++;

            size_t val_len = (size_t)(val_end - val_start);
            if (val_len >= value_size) val_len = value_size - 1;

            memcpy(value, val_start, val_len);
            value[val_len] = '\0';
            return 0;
        }

        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return -1;
}

/* Parse coordinate string "lat,lon" - delegates to shared library */
static int parse_coord(const char *str, double *lat, double *lon) {
    SHCoord coord;
    if (sh_parse_coord(str, &coord) != 0) return -1;
    *lat = coord.lat;
    *lon = coord.lon;
    return 0;
}

/* Parse profile string */
static VLProfile parse_profile(const char *str) {
    if (!str || !*str) return VL_PROFILE_CAR;

    if (strcmp(str, "car") == 0) return VL_PROFILE_CAR;
    if (strcmp(str, "truck") == 0) return VL_PROFILE_TRUCK;
    if (strcmp(str, "bike") == 0 || strcmp(str, "bicycle") == 0) return VL_PROFILE_BIKE;
    if (strcmp(str, "foot") == 0 || strcmp(str, "pedestrian") == 0 || strcmp(str, "walk") == 0) return VL_PROFILE_FOOT;

    return VL_PROFILE_CAR;
}

/* Parse mode/weight string */
static VLWeightType parse_mode(const char *str) {
    if (!str || !*str) return VL_WEIGHT_DURATION;

    if (strcmp(str, "fastest") == 0 || strcmp(str, "duration") == 0) return VL_WEIGHT_DURATION;
    if (strcmp(str, "shortest") == 0 || strcmp(str, "distance") == 0) return VL_WEIGHT_DISTANCE;

    return VL_WEIGHT_DURATION;
}

/* Parse boolean string */
static int parse_bool(const char *str, int default_val) {
    if (!str || !*str) return default_val;

    if (strcmp(str, "true") == 0 || strcmp(str, "1") == 0 || strcmp(str, "yes") == 0) return 1;
    if (strcmp(str, "false") == 0 || strcmp(str, "0") == 0 || strcmp(str, "no") == 0) return 0;

    return default_val;
}


/* ============================================================================
 * Route Parameter Parsing
 * ============================================================================ */

int vl_api_parse_route_params(const char *query,
                              const char *body, size_t body_len,
                              const char *method,
                              VLAPIRouteParams *params,
                              char *error_msg, size_t error_msg_len) {
    if (!params) return -1;

    memset(params, 0, sizeof(*params));
    params->profile = VL_PROFILE_CAR;
    params->weight = VL_WEIGHT_DURATION;
    /* Geometry on unless asked otherwise: that is what the HTTP server has
     * always returned, and a route with no shape is not much of a route. */
    params->include_geometry = 1;

    int is_post = method && strcmp(method, "POST") == 0;

    if (is_post) {
        /* A POST with no body is a malformed request, not a request to fall
         * back to the query string. */
        if (!body || body_len == 0) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Empty request body");
            return -1;
        }

        /* Parse JSON body using sh_json. body_len is passed in rather than
         * measured with strlen(): a transport hands us a length, and the
         * bytes behind it are not guaranteed to be NUL-terminated. */
        size_t arena_size = body_len * 8;
        if (arena_size < 4096) arena_size = 4096;
        SHArena *arena = sh_arena_create(arena_size);
        if (!arena) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Memory allocation failed");
            return -1;
        }

        ShJsonValue *root = NULL;
        ShJsonStatus json_status = sh_json_parse(body, body_len, arena, &root);
        if (json_status != SH_JSON_OK) {
            sh_arena_free(arena);
            if (error_msg) snprintf(error_msg, error_msg_len, "Invalid JSON: %s",
                                    sh_json_status_str(json_status));
            return -1;
        }

        /* Extract 'from' field */
        const char *from_str = sh_json_as_string(sh_json_get(root, "from"), NULL);
        if (!from_str) {
            sh_arena_free(arena);
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'from' field");
            return -1;
        }
        if (parse_coord(from_str, &params->from_lat, &params->from_lon) != 0) {
            sh_arena_free(arena);
            if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'from' coordinate");
            return -1;
        }

        /* Extract 'to' field */
        const char *to_str = sh_json_as_string(sh_json_get(root, "to"), NULL);
        if (!to_str) {
            sh_arena_free(arena);
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'to' field");
            return -1;
        }
        if (parse_coord(to_str, &params->to_lat, &params->to_lon) != 0) {
            sh_arena_free(arena);
            if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'to' coordinate");
            return -1;
        }

        /* Optional: profile */
        const char *profile_str = sh_json_as_string(sh_json_get(root, "profile"), NULL);
        if (profile_str) {
            params->profile = parse_profile(profile_str);
        }

        /* Optional: mode */
        const char *mode_str = sh_json_as_string(sh_json_get(root, "mode"), NULL);
        if (mode_str) {
            params->weight = parse_mode(mode_str);
        }

        /* Optional: geometry (boolean) */
        ShJsonValue *geom_val = sh_json_get(root, "geometry");
        if (geom_val) {
            params->include_geometry = sh_json_as_bool(geom_val, true) ? 1 : 0;
        }

        sh_arena_free(arena);
    } else {
        /* Parse query string */
        char value[256];

        if (get_query_param(query, "from", value, sizeof(value)) != 0) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'from' parameter");
            return -1;
        }
        if (parse_coord(value, &params->from_lat, &params->from_lon) != 0) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'from' coordinate (format: lat,lon)");
            return -1;
        }

        if (get_query_param(query, "to", value, sizeof(value)) != 0) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'to' parameter");
            return -1;
        }
        if (parse_coord(value, &params->to_lat, &params->to_lon) != 0) {
            if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'to' coordinate (format: lat,lon)");
            return -1;
        }

        if (get_query_param(query, "profile", value, sizeof(value)) == 0) {
            params->profile = parse_profile(value);
        }

        if (get_query_param(query, "mode", value, sizeof(value)) == 0) {
            params->weight = parse_mode(value);
        }

        if (get_query_param(query, "geometry", value, sizeof(value)) == 0) {
            params->include_geometry = parse_bool(value, 1);
        }
    }

    return 0;
}

/* ============================================================================
 * Individual Handlers
 * ============================================================================ */

char *vl_api_health(VLAPIContext *ctx, size_t *out_len) {
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_kv_string(&jw, "status", "healthy");
    sh_json_write_kv_string(&jw, "service", ctx ? ctx->name : "velo-route-server");
    sh_json_write_kv_string(&jw, "version", vl_version());
    sh_json_write_object_end(&jw);

    if (sh_json_writer_error(&jw)) {
        sh_json_buf_free(&jb);
        if (out_len) *out_len = 0;
        return NULL;
    }

    char *result = sh_json_buf_take(&jb);
    if (out_len) *out_len = result ? strlen(result) : 0;
    return result;
}

char *vl_api_stats(VLAPIContext *ctx, size_t *out_len) {
    if (!ctx || !ctx->graph) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    VLGraph *g = ctx->graph;

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_kv_string(&jw, "graph_path", ctx->graph_path);
    sh_json_write_kv_int(&jw, "num_nodes", (int64_t)g->num_nodes);
    sh_json_write_kv_int(&jw, "num_edges", (int64_t)g->num_edges);
    sh_json_write_kv_bool(&jw, "landmarks_enabled", ctx->landmarks != NULL);
    sh_json_write_kv_int(&jw, "landmark_count", ctx->landmark_count);

    sh_json_write_key(&jw, "bbox");
    sh_json_write_object_start(&jw);
    sh_json_write_kv_double_fmt(&jw, "min_lat", g->bbox_min.lat, 6);
    sh_json_write_kv_double_fmt(&jw, "min_lon", g->bbox_min.lon, 6);
    sh_json_write_kv_double_fmt(&jw, "max_lat", g->bbox_max.lat, 6);
    sh_json_write_kv_double_fmt(&jw, "max_lon", g->bbox_max.lon, 6);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);

    if (sh_json_writer_error(&jw)) {
        sh_json_buf_free(&jb);
        if (out_len) *out_len = 0;
        return NULL;
    }

    char *result = sh_json_buf_take(&jb);
    if (out_len) *out_len = result ? strlen(result) : 0;
    return result;
}

char *vl_api_route(VLAPIContext *ctx,
                   const VLAPIRouteParams *params,
                   int *status_code,
                   size_t *out_len) {
    if (!ctx || !ctx->graph || !params) {
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    VLGraph *g = ctx->graph;

    /* Validate coordinates are within graph bounds */
    if (params->from_lat < g->bbox_min.lat || params->from_lat > g->bbox_max.lat ||
        params->from_lon < g->bbox_min.lon || params->from_lon > g->bbox_max.lon) {
        if (status_code) *status_code = 400;
        return make_error_response("Origin coordinate outside graph bounds", out_len);
    }
    if (params->to_lat < g->bbox_min.lat || params->to_lat > g->bbox_max.lat ||
        params->to_lon < g->bbox_min.lon || params->to_lon > g->bbox_max.lon) {
        if (status_code) *status_code = 400;
        return make_error_response("Destination coordinate outside graph bounds", out_len);
    }

    /* Set up routing options */
    VLRouteOptions opts = {0};
    opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    opts.weight = params->weight;
    opts.profile = params->profile;
    opts.include_geometry = params->include_geometry;

    VLCoord from = {params->from_lat, params->from_lon};
    VLCoord to = {params->to_lat, params->to_lon};

    VLRoute route;
    VLStatus vl_status;

    if (ctx->landmarks) {
        /* Use landmarks for faster routing */
        uint32_t from_node = vl_graph_nearest_node_grid(g, from);
        uint32_t to_node = vl_graph_nearest_node_grid(g, to);

        if (from_node == VL_INVALID_NODE) {
            if (status_code) *status_code = 400;
            return make_error_response("Could not find road near origin", out_len);
        }
        if (to_node == VL_INVALID_NODE) {
            if (status_code) *status_code = 400;
            return make_error_response("Could not find road near destination", out_len);
        }

        vl_status = vl_route_astar_landmarks_bidir(g, ctx->landmarks,
                                                    from_node, to_node,
                                                    &opts, &route);
    } else {
        vl_status = vl_route_coords(g, from, to, &opts, &route);
    }

    if (vl_status != VL_OK) {
        const char *msg = "Routing failed";
        switch (vl_status) {
            case VL_ERROR_NO_ROUTE: msg = "No route found"; break;
            case VL_ERROR_NODE_NOT_FOUND: msg = "Could not find road near coordinate"; break;
            default: break;
        }
        if (status_code) *status_code = 404;
        return make_error_response(msg, out_len);
    }

    /* Encode polyline if geometry requested */
    char *polyline = NULL;
    if (params->include_geometry && route.num_coords > 0) {
        size_t max_len = sh_polyline_max_encoded_size(route.num_coords);
        polyline = malloc(max_len);
        if (polyline) {
            double *coords = NULL;
            if ((size_t)route.num_coords <= SIZE_MAX / (2 * sizeof(double))) {
                coords = malloc((size_t)route.num_coords * 2 * sizeof(double));
            }
            if (coords) {
                for (int i = 0; i < route.num_coords; i++) {
                    coords[i * 2] = route.coords[i].lat;
                    coords[i * 2 + 1] = route.coords[i].lon;
                }
                sh_polyline_encode(coords, route.num_coords, 5, polyline, max_len);
                free(coords);
            } else {
                free(polyline);
                polyline = NULL;
            }
        }
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    const char *profile_str = "car";
    switch (params->profile) {
        case VL_PROFILE_TRUCK: profile_str = "truck"; break;
        case VL_PROFILE_BIKE: profile_str = "bike"; break;
        case VL_PROFILE_FOOT: profile_str = "foot"; break;
        default: break;
    }

    const char *mode_str = params->weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest";

    sh_json_write_object_start(&jw);
    sh_json_write_kv_string(&jw, "status", "ok");

    sh_json_write_key(&jw, "route");
    sh_json_write_object_start(&jw);
    sh_json_write_kv_double_fmt(&jw, "distance", route.distance_m, 2);
    sh_json_write_kv_double_fmt(&jw, "duration", route.duration_s, 2);
    sh_json_write_kv_string(&jw, "profile", profile_str);
    sh_json_write_kv_string(&jw, "mode", mode_str);

    /* from: [lat, lon] */
    sh_json_write_key(&jw, "from");
    sh_json_write_array_start(&jw);
    sh_json_write_double_fmt(&jw, params->from_lat, 6);
    sh_json_write_double_fmt(&jw, params->from_lon, 6);
    sh_json_write_array_end(&jw);

    /* to: [lat, lon] */
    sh_json_write_key(&jw, "to");
    sh_json_write_array_start(&jw);
    sh_json_write_double_fmt(&jw, params->to_lat, 6);
    sh_json_write_double_fmt(&jw, params->to_lon, 6);
    sh_json_write_array_end(&jw);

    /* geometry (optional) - ShJsonWriter handles escaping */
    if (polyline) {
        sh_json_write_kv_string(&jw, "geometry", polyline);
    }

    sh_json_write_object_end(&jw);  /* Close route */

    /* meta object */
    sh_json_write_key(&jw, "meta");
    sh_json_write_object_start(&jw);
    sh_json_write_kv_int(&jw, "nodes_explored", (int64_t)route.nodes_explored);
    sh_json_write_kv_double_fmt(&jw, "search_time_ms", route.search_time_ms, 2);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);  /* Close root */

    if (polyline) free(polyline);
    vl_free_route(&route);

    if (sh_json_writer_error(&jw)) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    char *result = sh_json_buf_take(&jb);
    if (status_code) *status_code = 200;
    if (out_len) *out_len = result ? strlen(result) : 0;
    return result;
}

/* ============================================================================
 * Main Handler
 * ============================================================================ */

int vl_api_handle(void *ctx_void,
                  const ShApiRequest *req,
                  ShApiResponse *resp) {
    VLAPIContext *ctx = (VLAPIContext *)ctx_void;

    if (!ctx || !req || !resp || !req->path) return -1;

    memset(resp, 0, sizeof(*resp));
    resp->content_type = "application/json";

    /* Health check */
    if (strcmp(req->path, "/api/v1/health") == 0) {
        char *body = vl_api_health(ctx, &resp->body_len);
        if (!body) return sh_api_response_error(resp, 500, "Internal error");
        resp->body = (uint8_t *)body;
        resp->status_code = 200;
        return 0;
    }

    /* Stats */
    if (strcmp(req->path, "/api/v1/stats") == 0) {
        char *body;
        if (!ctx->graph) {
            return sh_api_response_error(resp, 503, "Graph not loaded");
        }
        body = vl_api_stats(ctx, &resp->body_len);
        if (!body) return sh_api_response_error(resp, 500, "Internal error");
        resp->body = (uint8_t *)body;
        resp->status_code = 200;
        return 0;
    }

    /* Route */
    if (strcmp(req->path, "/api/v1/route") == 0) {
        VLAPIRouteParams params;
        char error_msg[256] = {0};
        int status = 500;
        char *body;

        if (!ctx->graph) {
            return sh_api_response_error(resp, 503, "Graph not loaded");
        }

        if (vl_api_parse_route_params(req->query, req->body, req->body_len,
                                      req->method, &params,
                                      error_msg, sizeof(error_msg)) != 0) {
            return sh_api_response_error(resp, 400,
                                         error_msg[0] ? error_msg
                                                      : "Invalid request");
        }

        /* vl_api_route() reports its own status -- 400 for a coordinate
         * outside the graph or with no road near it, 404 for no route, 200
         * otherwise -- and on failure its body is already {"error": "..."},
         * the same shape sh_api_response_error() produces. Passing it through
         * keeps the specific message ("Origin coordinate outside graph
         * bounds") that a generic one would throw away. */
        body = vl_api_route(ctx, &params, &status, &resp->body_len);
        if (!body) {
            return sh_api_response_error(resp, status ? status : 500,
                                         "Routing failed");
        }

        resp->body = (uint8_t *)body;
        resp->status_code = status;
        return 0;
    }

    /* 404 Not Found */
    return sh_api_response_error(resp, 404, "Not found");
}
