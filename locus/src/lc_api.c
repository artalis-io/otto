/*
 * Locus API Handler Implementation
 *
 * Transport-agnostic request handling for geocoding operations.
 */

#include "lc_api.h"
#include "lc_mmap.h"
#include "sh_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

/* ============================================================================
 * API Context
 * ============================================================================ */

struct LCAPIContext {
    LCIndex *index;         /* Geocoding index (not owned) */
    char data_path[512];    /* Path for stats reporting */
    char name[64];          /* Server name for health check */
};

void lc_api_config_init(LCAPIConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->name = "locus";
}

LCAPIContext *lc_api_create(LCIndex *index, const LCAPIConfig *config) {
    if (!index) return NULL;

    LCAPIContext *ctx = calloc(1, sizeof(LCAPIContext));
    if (!ctx) return NULL;

    ctx->index = index;

    if (config) {
        if (config->data_path) {
            strncpy(ctx->data_path, config->data_path, sizeof(ctx->data_path) - 1);
            ctx->data_path[sizeof(ctx->data_path) - 1] = '\0';
        }
        if (config->name) {
            strncpy(ctx->name, config->name, sizeof(ctx->name) - 1);
            ctx->name[sizeof(ctx->name) - 1] = '\0';
        }
    }

    if (ctx->name[0] == '\0') {
        strncpy(ctx->name, "locus", sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    }

    return ctx;
}

void lc_api_free(LCAPIContext *ctx) {
    free(ctx);
}

LCIndex *lc_api_get_index(LCAPIContext *ctx) {
    return ctx ? ctx->index : NULL;
}

/* ============================================================================
 * JSON Helpers
 * ============================================================================ */

/* Escape a string for JSON output */
static void json_escape(const char *src, char *dst, size_t dst_size) {
    if (!src || !dst || dst_size == 0) {
        if (dst && dst_size > 0) dst[0] = '\0';
        return;
    }

    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c < 0x20) {
            /* Skip other control characters */
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

/* Parse a query parameter value from query string */
static int parse_query_param(const char *query, const char *name,
                             char *value, size_t value_size) {
    if (!query || !name || !value || value_size == 0) {
        if (value && value_size > 0) value[0] = '\0';
        return 0;
    }

    size_t name_len = strlen(name);
    const char *p = query;

    while (*p) {
        /* Check if this is our parameter */
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            p += name_len + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < value_size - 1) {
                value[i++] = *p++;
            }
            value[i] = '\0';
            return 1;
        }

        /* Skip to next parameter */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    value[0] = '\0';
    return 0;
}

/* ============================================================================
 * Individual Handlers
 * ============================================================================ */

char *lc_api_health(LCAPIContext *ctx, size_t *out_len) {
    const char *name = ctx ? ctx->name : "locus";
    const char *version = lc_version();

    char *json = malloc(256);
    if (!json) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    int len = snprintf(json, 256,
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"%s\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        name, version ? version : "unknown");

    if (out_len) *out_len = (size_t)len;
    return json;
}

char *lc_api_stats(LCAPIContext *ctx, size_t *out_len) {
    if (!ctx || !ctx->index) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    SHBBox bounds = lc_index_bounds(ctx->index);

    char *json = malloc(1024);
    if (!json) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    int len = snprintf(json, 1024,
        "{\n"
        "  \"entities\": %u,\n"
        "  \"memory_mb\": %.2f,\n"
        "  \"bounds\": {\n"
        "    \"min_lat\": %.6f,\n"
        "    \"min_lon\": %.6f,\n"
        "    \"max_lat\": %.6f,\n"
        "    \"max_lon\": %.6f\n"
        "  }\n"
        "}\n",
        lc_index_entity_count(ctx->index),
        (double)lc_index_memory_usage(ctx->index) / (1024.0 * 1024.0),
        bounds.min_lat, bounds.min_lon,
        bounds.max_lat, bounds.max_lon);

    if (out_len) *out_len = (size_t)len;
    return json;
}

char *lc_api_search(LCAPIContext *ctx,
                    const char *query,
                    int limit,
                    int *status_code,
                    size_t *out_len) {
    if (!ctx || !ctx->index) {
        if (status_code) *status_code = 503;
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (!query || strlen(query) == 0) {
        if (status_code) *status_code = 400;
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Clamp limit */
    if (limit <= 0) limit = 10;
    if (limit > 100) limit = 100;

    /* Perform search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = limit;

    LCSearchResult result;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    LCStatus status = lc_search(ctx->index, query, &opts, &result);
    gettimeofday(&end, NULL);
    double took_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_usec - start.tv_usec) / 1000.0;

    if (status != LC_OK) {
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Build JSON response */
    char *json = malloc(64 * 1024);  /* 64KB buffer */
    if (!json) {
        lc_search_result_free(&result);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    char escaped_query[512];
    json_escape(query, escaped_query, sizeof(escaped_query));

    int offset = snprintf(json, 64 * 1024,
                          "{\n"
                          "  \"query\": \"%s\",\n"
                          "  \"total\": %zu,\n"
                          "  \"took_ms\": %.2f,\n"
                          "  \"results\": [",
                          escaped_query, result.total_matches, took_ms);

    for (size_t i = 0; i < result.num_results; i++) {
        uint32_t eid = result.matches[i].entity_id;
        char escaped_name[512] = "";
        const char *osm_type = "node";
        uint64_t osm_id = 0;
        const char *fclass_str = "unknown";
        double lat = 0, lon = 0;

        if (ctx->index->mmap_idx) {
            /* v4 mmap path */
            const char *name = lc_mmap_entity_name(ctx->index->mmap_idx, eid);
            if (name) json_escape(name, escaped_name, sizeof(escaped_name));

            LCEntityType type = lc_mmap_entity_type(ctx->index->mmap_idx, eid);
            if (type == LC_ENTITY_WAY) osm_type = "way";
            else if (type == LC_ENTITY_RELATION) osm_type = "relation";

            osm_id = lc_mmap_entity_osm_id(ctx->index->mmap_idx, eid);
            fclass_str = lc_class_string(lc_mmap_entity_fclass(ctx->index->mmap_idx, eid));
            SHCoord c = lc_mmap_entity_centroid(ctx->index->mmap_idx, eid);
            lat = c.lat;
            lon = c.lon;
        } else {
            /* Entity store path */
            const LCEntity *e = lc_search_get_entity(ctx->index, &result.matches[i]);
            if (!e) continue;

            if (e->name) json_escape(e->name, escaped_name, sizeof(escaped_name));
            if (e->type == LC_ENTITY_WAY) osm_type = "way";
            else if (e->type == LC_ENTITY_RELATION) osm_type = "relation";

            osm_id = e->osm_id;
            fclass_str = lc_class_string(e->fclass);
            lat = e->centroid.lat;
            lon = e->centroid.lon;
        }

        offset += snprintf(json + offset, 64 * 1024 - offset,
                          "%s\n    {\n"
                          "      \"osm_id\": %lu,\n"
                          "      \"osm_type\": \"%s\",\n"
                          "      \"name\": \"%s\",\n"
                          "      \"class\": \"%s\",\n"
                          "      \"lat\": %.6f,\n"
                          "      \"lon\": %.6f,\n"
                          "      \"score\": %.4f\n"
                          "    }",
                          i > 0 ? "," : "",
                          (unsigned long)osm_id,
                          osm_type,
                          escaped_name,
                          fclass_str,
                          lat,
                          lon,
                          result.matches[i].score);
    }

    offset += snprintf(json + offset, 64 * 1024 - offset, "\n  ]\n}\n");

    lc_search_result_free(&result);

    if (status_code) *status_code = 200;
    if (out_len) *out_len = (size_t)offset;
    return json;
}

char *lc_api_autocomplete(LCAPIContext *ctx,
                          const char *prefix,
                          int limit,
                          int *status_code,
                          size_t *out_len) {
    if (!ctx || !ctx->index) {
        if (status_code) *status_code = 503;
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (!prefix || strlen(prefix) == 0) {
        if (status_code) *status_code = 400;
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Clamp limit */
    if (limit <= 0) limit = 10;
    if (limit > 20) limit = 20;

    LCSearchResult result;
    lc_autocomplete(ctx->index, prefix, limit, &result);

    /* Build simple suggestions array */
    char *json = malloc(16 * 1024);
    if (!json) {
        lc_search_result_free(&result);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    int offset = snprintf(json, 16 * 1024, "[\n");

    for (size_t i = 0; i < result.num_results; i++) {
        const char *name = NULL;

        if (ctx->index->mmap_idx) {
            name = lc_mmap_entity_name(ctx->index->mmap_idx, result.matches[i].entity_id);
        } else {
            const LCEntity *e = lc_search_get_entity(ctx->index, &result.matches[i]);
            if (e) name = e->name;
        }

        if (!name) continue;

        char escaped[512];
        json_escape(name, escaped, sizeof(escaped));

        offset += snprintf(json + offset, 16 * 1024 - offset,
                          "%s  \"%s\"",
                          i > 0 ? ",\n" : "",
                          escaped);
    }

    offset += snprintf(json + offset, 16 * 1024 - offset, "\n]\n");

    lc_search_result_free(&result);

    if (status_code) *status_code = 200;
    if (out_len) *out_len = (size_t)offset;
    return json;
}

char *lc_api_reverse(LCAPIContext *ctx,
                     double lat,
                     double lon,
                     int *status_code,
                     size_t *out_len) {
    if (!ctx || !ctx->index) {
        if (status_code) *status_code = 503;
        if (out_len) *out_len = 0;
        return NULL;
    }

    /* Validate coordinates */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        if (status_code) *status_code = 400;
        if (out_len) *out_len = 0;
        return NULL;
    }

    SHCoord coord = {.lat = lat, .lon = lon};
    LCReverseResult result;
    LCStatus status = lc_reverse(ctx->index, coord, NULL, &result);

    if (status != LC_OK) {
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    char address[512] = "";
    lc_format_address(&result, address, sizeof(address));

    char escaped_address[1024];
    json_escape(address, escaped_address, sizeof(escaped_address));

    char *json = malloc(4 * 1024);
    if (!json) {
        lc_reverse_result_free(&result);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    int offset = snprintf(json, 4 * 1024,
                          "{\n"
                          "  \"lat\": %.6f,\n"
                          "  \"lon\": %.6f,\n"
                          "  \"display_name\": \"%s\",\n"
                          "  \"distance_m\": %.1f",
                          lat, lon, escaped_address, result.distance_m);

    if (result.place && result.place->name) {
        char escaped[512];
        json_escape(result.place->name, escaped, sizeof(escaped));
        offset += snprintf(json + offset, 4 * 1024 - offset,
                          ",\n  \"place\": \"%s\"", escaped);
    }

    if (result.street && result.street->name) {
        char escaped[512];
        json_escape(result.street->name, escaped, sizeof(escaped));
        offset += snprintf(json + offset, 4 * 1024 - offset,
                          ",\n  \"street\": \"%s\"", escaped);
    }

    offset += snprintf(json + offset, 4 * 1024 - offset, "\n}\n");

    lc_reverse_result_free(&result);

    if (status_code) *status_code = 200;
    if (out_len) *out_len = (size_t)offset;
    return json;
}

/* ============================================================================
 * Main Request Handler
 * ============================================================================ */

void lc_api_response_free(LCAPIResponse *resp) {
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}

/* Helper to set error response */
static void set_error_response(LCAPIResponse *resp, int status_code, const char *message) {
    resp->status_code = status_code;
    resp->content_type = "application/json";

    char escaped[256];
    json_escape(message, escaped, sizeof(escaped));

    char *json = malloc(512);
    if (json) {
        int len = snprintf(json, 512, "{\"error\": \"%s\"}\n", escaped);
        resp->body = (uint8_t *)json;
        resp->body_len = (size_t)len;
    } else {
        resp->body = NULL;
        resp->body_len = 0;
    }
}

/* Check if path matches (simple prefix match) */
static int path_matches(const char *path, const char *pattern) {
    if (!path || !pattern) return 0;
    size_t pattern_len = strlen(pattern);
    return strncmp(path, pattern, pattern_len) == 0 &&
           (path[pattern_len] == '\0' || path[pattern_len] == '?');
}

int lc_api_handle(LCAPIContext *ctx,
                  const LCAPIRequest *req,
                  LCAPIResponse *resp) {
    if (!req || !resp) return -1;

    /* Initialize response */
    memset(resp, 0, sizeof(*resp));
    resp->content_type = "application/json";

    /* Route based on path */
    if (path_matches(req->path, "/api/v1/health")) {
        char *json = lc_api_health(ctx, &resp->body_len);
        if (json) {
            resp->status_code = 200;
            resp->body = (uint8_t *)json;
        } else {
            set_error_response(resp, 500, "Internal error");
        }
        return 0;
    }

    if (path_matches(req->path, "/api/v1/stats")) {
        if (!ctx || !ctx->index) {
            set_error_response(resp, 503, "Index not loaded");
            return 0;
        }
        char *json = lc_api_stats(ctx, &resp->body_len);
        if (json) {
            resp->status_code = 200;
            resp->body = (uint8_t *)json;
        } else {
            set_error_response(resp, 500, "Internal error");
        }
        return 0;
    }

    if (path_matches(req->path, "/api/v1/search")) {
        char query[256] = "";
        char limit_str[16] = "10";
        parse_query_param(req->query, "q", query, sizeof(query));
        parse_query_param(req->query, "limit", limit_str, sizeof(limit_str));

        if (strlen(query) == 0) {
            set_error_response(resp, 400, "Missing 'q' parameter");
            return 0;
        }

        int status_code;
        char *json = lc_api_search(ctx, query, atoi(limit_str), &status_code, &resp->body_len);
        if (json) {
            resp->status_code = status_code;
            resp->body = (uint8_t *)json;
        } else {
            set_error_response(resp, status_code,
                status_code == 503 ? "Index not loaded" : "Search failed");
        }
        return 0;
    }

    if (path_matches(req->path, "/api/v1/autocomplete")) {
        char query[256] = "";
        char limit_str[16] = "10";
        parse_query_param(req->query, "q", query, sizeof(query));
        parse_query_param(req->query, "limit", limit_str, sizeof(limit_str));

        if (strlen(query) == 0) {
            set_error_response(resp, 400, "Missing 'q' parameter");
            return 0;
        }

        int status_code;
        char *json = lc_api_autocomplete(ctx, query, atoi(limit_str), &status_code, &resp->body_len);
        if (json) {
            resp->status_code = status_code;
            resp->body = (uint8_t *)json;
        } else {
            set_error_response(resp, status_code,
                status_code == 503 ? "Index not loaded" : "Autocomplete failed");
        }
        return 0;
    }

    if (path_matches(req->path, "/api/v1/reverse")) {
        char lat_str[32] = "";
        char lon_str[32] = "";
        parse_query_param(req->query, "lat", lat_str, sizeof(lat_str));
        parse_query_param(req->query, "lon", lon_str, sizeof(lon_str));

        if (strlen(lat_str) == 0 || strlen(lon_str) == 0) {
            set_error_response(resp, 400, "Missing 'lat' or 'lon' parameter");
            return 0;
        }

        double lat = sh_parse_double(lat_str, NAN, -90.0, 90.0);
        double lon = sh_parse_double(lon_str, NAN, -180.0, 180.0);
        if (isnan(lat) || isnan(lon)) {
            set_error_response(resp, 400, "Invalid coordinates");
            return 0;
        }

        int status_code;
        char *json = lc_api_reverse(ctx, lat, lon, &status_code, &resp->body_len);
        if (json) {
            resp->status_code = status_code;
            resp->body = (uint8_t *)json;
        } else {
            if (status_code == 400) {
                set_error_response(resp, 400, "Invalid coordinates");
            } else {
                set_error_response(resp, status_code,
                    status_code == 503 ? "Index not loaded" : "Reverse geocoding failed");
            }
        }
        return 0;
    }

    /* Unknown path */
    set_error_response(resp, 404, "Not found");
    return 0;
}
