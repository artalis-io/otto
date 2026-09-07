/*
 * Locus API Handler Implementation
 *
 * Transport-agnostic request handling for geocoding operations.
 */

#include "lc_api.h"
#include "lc_mmap.h"
#include "sh_args.h"
#include "sh_json.h"
#include "sh_query.h"
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

    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = (size_t)limit;

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

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "query");
    sh_json_write_string(&jw, query);
    sh_json_write_key(&jw, "total");
    sh_json_write_int(&jw, (int64_t)result.total_matches);
    sh_json_write_key(&jw, "took_ms");
    sh_json_write_double(&jw, took_ms);
    sh_json_write_key(&jw, "results");
    sh_json_write_array_start(&jw);

    for (size_t i = 0; i < result.num_results; i++) {
        uint32_t eid = result.matches[i].entity_id;
        const char *name = NULL;
        const char *osm_type = "node";
        uint64_t osm_id = 0;
        const char *fclass_str = "unknown";
        double lat = 0, lon = 0;

        if (ctx->index->mmap_idx) {
            /* v4 mmap path */
            name = lc_mmap_entity_name(ctx->index->mmap_idx, eid);

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

            name = e->name;
            if (e->type == LC_ENTITY_WAY) osm_type = "way";
            else if (e->type == LC_ENTITY_RELATION) osm_type = "relation";

            osm_id = e->osm_id;
            fclass_str = lc_class_string(e->fclass);
            lat = e->centroid.lat;
            lon = e->centroid.lon;
        }

        sh_json_write_object_start(&jw);
        sh_json_write_key(&jw, "osm_id");
        sh_json_write_int(&jw, (int64_t)osm_id);
        sh_json_write_key(&jw, "osm_type");
        sh_json_write_string(&jw, osm_type);
        sh_json_write_key(&jw, "name");
        sh_json_write_string(&jw, name ? name : "");
        sh_json_write_key(&jw, "class");
        sh_json_write_string(&jw, fclass_str);
        sh_json_write_key(&jw, "lat");
        sh_json_write_double(&jw, lat);
        sh_json_write_key(&jw, "lon");
        sh_json_write_double(&jw, lon);
        sh_json_write_key(&jw, "score");
        sh_json_write_double(&jw, result.matches[i].score);
        sh_json_write_object_end(&jw);
    }

    sh_json_write_array_end(&jw);
    sh_json_write_object_end(&jw);

    lc_search_result_free(&result);

    if (jw.error) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (status_code) *status_code = 200;
    if (out_len) *out_len = jb.len;
    return sh_json_buf_take(&jb);
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

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_array_start(&jw);

    for (size_t i = 0; i < result.num_results; i++) {
        const char *name = NULL;

        if (ctx->index->mmap_idx) {
            name = lc_mmap_entity_name(ctx->index->mmap_idx,
                                       result.matches[i].entity_id);
        } else {
            const LCEntity *e = lc_search_get_entity(ctx->index, &result.matches[i]);
            if (e) name = e->name;
        }

        if (!name) continue;

        sh_json_write_string(&jw, name);
    }

    sh_json_write_array_end(&jw);

    lc_search_result_free(&result);

    if (jw.error) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (status_code) *status_code = 200;
    if (out_len) *out_len = jb.len;
    return sh_json_buf_take(&jb);
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

    /* Validate coordinates. isnan() matters: a NaN fails every comparison
     * below, so without it an unparseable "lat" would sail straight through. */
    if (isnan(lat) || isnan(lon) ||
        lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
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

    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "lat");
    sh_json_write_double(&jw, lat);
    sh_json_write_key(&jw, "lon");
    sh_json_write_double(&jw, lon);
    sh_json_write_key(&jw, "display_name");
    sh_json_write_string(&jw, address);
    sh_json_write_key(&jw, "distance_m");
    sh_json_write_double(&jw, result.distance_m);

    if (result.place && result.place->name) {
        sh_json_write_key(&jw, "place");
        sh_json_write_string(&jw, result.place->name);
    }

    if (result.street && result.street->name) {
        sh_json_write_key(&jw, "street");
        sh_json_write_string(&jw, result.street->name);
    }

    sh_json_write_object_end(&jw);

    lc_reverse_result_free(&result);

    if (jw.error) {
        sh_json_buf_free(&jb);
        if (status_code) *status_code = 500;
        if (out_len) *out_len = 0;
        return NULL;
    }

    if (status_code) *status_code = 200;
    if (out_len) *out_len = jb.len;
    return sh_json_buf_take(&jb);
}

/* ============================================================================
 * Main Request Handler
 * ============================================================================ */

/* Match a route. The query string never reaches here -- transports hand the
 * path and the query separately -- but a '?' is tolerated so a caller that
 * passes a whole request-target still routes correctly. */
static int path_matches(const char *path, const char *pattern) {
    if (!path || !pattern) return 0;
    size_t pattern_len = strlen(pattern);
    return strncmp(path, pattern, pattern_len) == 0 &&
           (path[pattern_len] == '\0' || path[pattern_len] == '?');
}

/* Map a helper's status_code onto a response. The helpers report 503 for "no
 * index", 400 for bad input and 500 for everything else; `what` names the
 * operation so the 500 says which one failed. */
static int fail(ShApiResponse *resp, int status_code, const char *what) {
    if (status_code == 503) {
        return sh_api_response_error(resp, 503, "Index not loaded");
    }
    if (status_code == 400) {
        return sh_api_response_error(resp, 400, "Invalid request");
    }
    return sh_api_response_error(resp, status_code ? status_code : 500, what);
}

int lc_api_handle(void *ctx_void,
                  const ShApiRequest *req,
                  ShApiResponse *resp) {
    LCAPIContext *ctx = (LCAPIContext *)ctx_void;

    if (!req || !resp || !req->path) return -1;

    memset(resp, 0, sizeof(*resp));

    if (path_matches(req->path, "/api/v1/health")) {
        size_t len = 0;
        char *json = lc_api_health(ctx, &len);
        if (!json) return sh_api_response_error(resp, 500, "Internal error");
        resp->status_code = 200;
        resp->content_type = "application/json";
        resp->body = (uint8_t *)json;
        resp->body_len = len;
        return 0;
    }

    if (path_matches(req->path, "/api/v1/stats")) {
        size_t len = 0;
        char *json;
        if (!ctx || !ctx->index) {
            return sh_api_response_error(resp, 503, "Index not loaded");
        }
        json = lc_api_stats(ctx, &len);
        if (!json) return sh_api_response_error(resp, 500, "Internal error");
        resp->status_code = 200;
        resp->content_type = "application/json";
        resp->body = (uint8_t *)json;
        resp->body_len = len;
        return 0;
    }

    if (path_matches(req->path, "/api/v1/search") ||
        path_matches(req->path, "/api/v1/autocomplete")) {
        int is_search = path_matches(req->path, "/api/v1/search");
        const char *q = req->query ? req->query : "";
        char text[256] = "";
        char limit_str[16] = "";
        int limit, status_code = 0;
        size_t len = 0;
        char *json;

        /* Decoded, not raw: "q=Monte%20Carlo" is a search for "Monte Carlo".
         * The limit is a number and has nothing to decode. */
        sh_query_get_str_decoded(q, "q", text, sizeof(text));
        if (text[0] == '\0') {
            return sh_api_response_error(resp, 400, "Missing 'q' parameter");
        }

        sh_query_get_str(q, "limit", limit_str, sizeof(limit_str));
        limit = sh_parse_int(limit_str[0] ? limit_str : "10", 10, 1, 100);

        json = is_search
            ? lc_api_search(ctx, text, limit, &status_code, &len)
            : lc_api_autocomplete(ctx, text, limit, &status_code, &len);
        if (!json) {
            return fail(resp, status_code,
                        is_search ? "Search failed" : "Autocomplete failed");
        }

        resp->status_code = 200;
        resp->content_type = "application/json";
        resp->body = (uint8_t *)json;
        resp->body_len = len;
        return 0;
    }

    if (path_matches(req->path, "/api/v1/reverse")) {
        const char *q = req->query ? req->query : "";
        char lat_str[32] = "";
        char lon_str[32] = "";
        double lat, lon;
        int status_code = 0;
        size_t len = 0;
        char *json;

        sh_query_get_str(q, "lat", lat_str, sizeof(lat_str));
        sh_query_get_str(q, "lon", lon_str, sizeof(lon_str));
        if (lat_str[0] == '\0' || lon_str[0] == '\0') {
            return sh_api_response_error(resp, 400,
                                         "Missing 'lat' or 'lon' parameter");
        }

        /* sh_parse_double yields NaN for text that is not a number, and
         * lc_api_reverse rejects NaN. atof() would have turned "north" into
         * 0.0 and quietly geocoded the Gulf of Guinea. */
        lat = sh_parse_double(lat_str, NAN, -90.0, 90.0);
        lon = sh_parse_double(lon_str, NAN, -180.0, 180.0);

        /* Validated here, before the index is consulted, so a malformed
         * coordinate is a 400 whether or not data is loaded -- the same order
         * the 'q' check above follows. lc_api_reverse() repeats the check for
         * callers that reach it directly. */
        if (isnan(lat) || isnan(lon) ||
            lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
            return sh_api_response_error(resp, 400, "Invalid coordinates");
        }

        json = lc_api_reverse(ctx, lat, lon, &status_code, &len);
        if (!json) {
            if (status_code == 400) {
                return sh_api_response_error(resp, 400, "Invalid coordinates");
            }
            return fail(resp, status_code, "Reverse geocoding failed");
        }

        resp->status_code = 200;
        resp->content_type = "application/json";
        resp->body = (uint8_t *)json;
        resp->body_len = len;
        return 0;
    }

    return sh_api_response_error(resp, 404, "Not found");
}
