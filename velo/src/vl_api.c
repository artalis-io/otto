/*
 * Velo API Handler - Transport-agnostic implementation
 *
 * This file contains the core request handling logic that can be used by
 * any transport layer (HTTP, WASM, etc.).
 */

#include "vl_api.h"
#include "sh_polyline.h"
#include "vl_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

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

void vl_api_response_free(VLAPIResponse *resp) {
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
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

/* Parse coordinate string "lat,lon" */
static int parse_coord(const char *str, double *lat, double *lon) {
    if (!str || !*str) return -1;

    char buf[64];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *comma = strchr(buf, ',');
    if (!comma) return -1;
    *comma = '\0';

    *lat = atof(buf);
    *lon = atof(comma + 1);

    /* Reject inf/NaN from malformed input */
    if (!isfinite(*lat) || !isfinite(*lon)) return -1;
    if (*lat < -90 || *lat > 90 || *lon < -180 || *lon > 180) return -1;

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

/* Simple JSON string extraction (for POST body parsing) */
static char *json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;

    /* Look for "key": "value" */
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *key_pos = strstr(json, search);
    if (!key_pos) return NULL;

    /* Find the colon */
    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return NULL;

    /* Skip whitespace */
    const char *p = colon + 1;
    while (*p && isspace((unsigned char)*p)) p++;

    /* Expect a quote */
    if (*p != '"') return NULL;
    p++;

    /* Find closing quote */
    const char *end = strchr(p, '"');
    if (!end) return NULL;

    size_t len = (size_t)(end - p);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

/* Simple JSON boolean extraction */
static int json_get_bool(const char *json, const char *key, int *value) {
    if (!json || !key || !value) return 0;

    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *key_pos = strstr(json, search);
    if (!key_pos) return 0;

    const char *colon = strchr(key_pos + strlen(search), ':');
    if (!colon) return 0;

    const char *p = colon + 1;
    while (*p && isspace((unsigned char)*p)) p++;

    if (strncmp(p, "true", 4) == 0) {
        *value = 1;
        return 1;
    } else if (strncmp(p, "false", 5) == 0) {
        *value = 0;
        return 1;
    }

    return 0;
}

/* ============================================================================
 * Route Parameter Parsing
 * ============================================================================ */

int vl_api_parse_route_params(const char *query, const char *body,
                              const char *method,
                              VLAPIRouteParams *params,
                              char *error_msg, size_t error_msg_len) {
    if (!params) return -1;

    memset(params, 0, sizeof(*params));
    params->profile = VL_PROFILE_CAR;
    params->weight = VL_WEIGHT_DURATION;
    params->include_geometry = 0;

    int is_post = method && strcmp(method, "POST") == 0;

    if (is_post && body) {
        /* Parse JSON body */
        char *from_str = json_get_string(body, "from");
        if (from_str) {
            if (parse_coord(from_str, &params->from_lat, &params->from_lon) != 0) {
                free(from_str);
                if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'from' coordinate");
                return -1;
            }
            free(from_str);
        } else {
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'from' field");
            return -1;
        }

        char *to_str = json_get_string(body, "to");
        if (to_str) {
            if (parse_coord(to_str, &params->to_lat, &params->to_lon) != 0) {
                free(to_str);
                if (error_msg) snprintf(error_msg, error_msg_len, "Invalid 'to' coordinate");
                return -1;
            }
            free(to_str);
        } else {
            if (error_msg) snprintf(error_msg, error_msg_len, "Missing 'to' field");
            return -1;
        }

        char *profile_str = json_get_string(body, "profile");
        if (profile_str) {
            params->profile = parse_profile(profile_str);
            free(profile_str);
        }

        char *mode_str = json_get_string(body, "mode");
        if (mode_str) {
            params->weight = parse_mode(mode_str);
            free(mode_str);
        }

        int geom;
        if (json_get_bool(body, "geometry", &geom)) {
            params->include_geometry = geom;
        }
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
            params->include_geometry = parse_bool(value, 0);
        }
    }

    return 0;
}

/* ============================================================================
 * Polyline Encoding Helper
 * ============================================================================ */

/* Escape backslashes in polyline for JSON output */
static char *json_escape_polyline(const char *polyline) {
    if (!polyline) return NULL;

    size_t len = strlen(polyline);
    size_t backslashes = 0;
    for (size_t i = 0; i < len; i++) {
        if (polyline[i] == '\\') backslashes++;
    }

    if (len > SIZE_MAX - backslashes - 1) return NULL;
    char *escaped = malloc(len + backslashes + 1);
    if (!escaped) return NULL;

    char *dst = escaped;
    for (size_t i = 0; i < len; i++) {
        if (polyline[i] == '\\') {
            *dst++ = '\\';
        }
        *dst++ = polyline[i];
    }
    *dst = '\0';

    return escaped;
}

/* ============================================================================
 * Individual Handlers
 * ============================================================================ */

char *vl_api_health(VLAPIContext *ctx, size_t *out_len) {
    char response[512];
    int n = snprintf(response, sizeof(response),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"%s\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        ctx ? ctx->name : "velo-route-server",
        vl_version());

    if (n < 0 || (size_t)n >= sizeof(response)) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    char *result = strdup(response);
    if (out_len) *out_len = result ? strlen(result) : 0;
    return result;
}

char *vl_api_stats(VLAPIContext *ctx, size_t *out_len) {
    if (!ctx || !ctx->graph) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    VLGraph *g = ctx->graph;
    char response[2048];
    int n = snprintf(response, sizeof(response),
        "{\n"
        "  \"graph_path\": \"%s\",\n"
        "  \"num_nodes\": %u,\n"
        "  \"num_edges\": %u,\n"
        "  \"landmarks_enabled\": %s,\n"
        "  \"landmark_count\": %d,\n"
        "  \"bbox\": {\n"
        "    \"min_lat\": %.6f,\n"
        "    \"min_lon\": %.6f,\n"
        "    \"max_lat\": %.6f,\n"
        "    \"max_lon\": %.6f\n"
        "  }\n"
        "}\n",
        ctx->graph_path,
        g->num_nodes,
        g->num_edges,
        ctx->landmarks ? "true" : "false",
        ctx->landmark_count,
        g->bbox_min.lat, g->bbox_min.lon,
        g->bbox_max.lat, g->bbox_max.lon);

    if (n < 0 || (size_t)n >= sizeof(response)) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    char *result = strdup(response);
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
        char *err = strdup("{\"error\": \"Origin coordinate outside graph bounds\"}");
        if (out_len) *out_len = err ? strlen(err) : 0;
        return err;
    }
    if (params->to_lat < g->bbox_min.lat || params->to_lat > g->bbox_max.lat ||
        params->to_lon < g->bbox_min.lon || params->to_lon > g->bbox_max.lon) {
        if (status_code) *status_code = 400;
        char *err = strdup("{\"error\": \"Destination coordinate outside graph bounds\"}");
        if (out_len) *out_len = err ? strlen(err) : 0;
        return err;
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
            char *err = strdup("{\"error\": \"Could not find road near origin\"}");
            if (out_len) *out_len = err ? strlen(err) : 0;
            return err;
        }
        if (to_node == VL_INVALID_NODE) {
            if (status_code) *status_code = 400;
            char *err = strdup("{\"error\": \"Could not find road near destination\"}");
            if (out_len) *out_len = err ? strlen(err) : 0;
            return err;
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
        char err[256];
        snprintf(err, sizeof(err), "{\"error\": \"%s\"}", msg);
        char *result = strdup(err);
        if (out_len) *out_len = result ? strlen(result) : 0;
        return result;
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

    /* Build JSON response */
    size_t resp_capacity = 4096 + (polyline ? strlen(polyline) * 2 : 0);
    char *response = malloc(resp_capacity);
    if (!response) {
        if (polyline) free(polyline);
        vl_free_route(&route);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    const char *profile_str = "car";
    switch (params->profile) {
        case VL_PROFILE_TRUCK: profile_str = "truck"; break;
        case VL_PROFILE_BIKE: profile_str = "bike"; break;
        case VL_PROFILE_FOOT: profile_str = "foot"; break;
        default: break;
    }

    const char *mode_str = params->weight == VL_WEIGHT_DISTANCE ? "shortest" : "fastest";

    size_t n = 0;
    int written = snprintf(response, resp_capacity,
        "{\n"
        "  \"status\": \"ok\",\n"
        "  \"route\": {\n"
        "    \"distance\": %.2f,\n"
        "    \"duration\": %.2f,\n"
        "    \"profile\": \"%s\",\n"
        "    \"mode\": \"%s\",\n"
        "    \"from\": [%.6f, %.6f],\n"
        "    \"to\": [%.6f, %.6f]",
        route.distance_m,
        route.duration_s,
        profile_str,
        mode_str,
        params->from_lat, params->from_lon,
        params->to_lat, params->to_lon);
    if (written > 0) n = (size_t)written;

    if (polyline && n < resp_capacity) {
        char *escaped = json_escape_polyline(polyline);
        if (escaped) {
            written = snprintf(response + n, resp_capacity - n,
                ",\n    \"geometry\": \"%s\"",
                escaped);
            if (written > 0) n += (size_t)written;
            free(escaped);
        }
    }

    if (n < resp_capacity) {
        written = snprintf(response + n, resp_capacity - n,
            "\n  },\n"
            "  \"meta\": {\n"
            "    \"nodes_explored\": %u,\n"
            "    \"search_time_ms\": %.2f\n"
            "  }\n"
            "}\n",
            route.nodes_explored,
            route.search_time_ms);
        if (written > 0) n += (size_t)written;
    }

    if (polyline) free(polyline);
    vl_free_route(&route);

    if (status_code) *status_code = 200;
    if (out_len) *out_len = n;
    return response;
}

/* ============================================================================
 * Main Handler
 * ============================================================================ */

int vl_api_handle(VLAPIContext *ctx,
                  const VLAPIRequest *req,
                  VLAPIResponse *resp) {
    if (!ctx || !req || !resp) return -1;

    memset(resp, 0, sizeof(*resp));
    resp->content_type = "application/json";

    /* Health check */
    if (strcmp(req->path, "/api/v1/health") == 0) {
        char *body = vl_api_health(ctx, &resp->body_len);
        resp->body = (uint8_t *)body;
        resp->status_code = body ? 200 : 500;
        return 0;
    }

    /* Stats */
    if (strcmp(req->path, "/api/v1/stats") == 0) {
        if (!ctx->graph) {
            resp->status_code = 503;
            resp->body = (uint8_t *)strdup("{\"error\": \"Graph not loaded\"}");
            resp->body_len = resp->body ? strlen((char *)resp->body) : 0;
            return 0;
        }
        char *body = vl_api_stats(ctx, &resp->body_len);
        resp->body = (uint8_t *)body;
        resp->status_code = body ? 200 : 500;
        return 0;
    }

    /* Route */
    if (strcmp(req->path, "/api/v1/route") == 0) {
        if (!ctx->graph) {
            resp->status_code = 503;
            resp->body = (uint8_t *)strdup("{\"error\": \"Graph not loaded\"}");
            resp->body_len = resp->body ? strlen((char *)resp->body) : 0;
            return 0;
        }

        VLAPIRouteParams params;
        char error_msg[256] = {0};

        if (vl_api_parse_route_params(req->query, req->body, req->method,
                                       &params, error_msg, sizeof(error_msg)) != 0) {
            resp->status_code = 400;
            char err[512];
            snprintf(err, sizeof(err), "{\"error\": \"%s\"}", error_msg);
            resp->body = (uint8_t *)strdup(err);
            resp->body_len = resp->body ? strlen((char *)resp->body) : 0;
            return 0;
        }

        int status;
        char *body = vl_api_route(ctx, &params, &status, &resp->body_len);
        resp->body = (uint8_t *)body;
        resp->status_code = status;
        return 0;
    }

    /* 404 Not Found */
    resp->status_code = 404;
    resp->body = (uint8_t *)strdup("{\"error\": \"Not found\"}");
    resp->body_len = resp->body ? strlen((char *)resp->body) : 0;
    return 0;
}
