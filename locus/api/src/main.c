/*
 * Locus API Server
 *
 * REST API for geocoding operations.
 * Uses mongoose for HTTP serving.
 */

#include "locus.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* ============================================================================
 * CORS Headers
 * ============================================================================ */

#define CORS_HEADERS "Access-Control-Allow-Origin: *\r\n"
#define JSON_CORS_HEADERS "Content-Type: application/json\r\n" CORS_HEADERS

/* ============================================================================
 * Global State
 * ============================================================================ */

static LCIndex *g_index = NULL;
static volatile sig_atomic_t s_signo = 0;

static void signal_handler(int signo) {
    s_signo = signo;
}

/* ============================================================================
 * JSON Helpers
 * ============================================================================ */

static void json_escape(const char *s, char *buf, size_t size) {
    size_t j = 0;
    for (size_t i = 0; s[i] && j < size - 1; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= size) break;
            buf[j++] = '\\';
        }
        buf[j++] = c;
    }
    buf[j] = '\0';
}

/* ============================================================================
 * Request Handlers
 * ============================================================================ */

static void handle_health(struct mg_connection *c) {
    mg_http_reply(c, 200, JSON_CORS_HEADERS,
                  "{\"status\":\"ok\",\"service\":\"locus\",\"version\":\"%s\"}\n",
                  lc_version());
}

static void handle_stats(struct mg_connection *c) {
    if (!g_index) {
        mg_http_reply(c, 503, JSON_CORS_HEADERS,
                      "{\"error\":\"Index not loaded\"}\n");
        return;
    }

    SHBBox bounds = lc_index_bounds(g_index);

    mg_http_reply(c, 200, JSON_CORS_HEADERS,
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
                  lc_index_entity_count(g_index),
                  (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0),
                  bounds.min_lat, bounds.min_lon,
                  bounds.max_lat, bounds.max_lon);
}

static void handle_search(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        mg_http_reply(c, 503, JSON_CORS_HEADERS,
                      "{\"error\":\"Index not loaded\"}\n");
        return;
    }

    /* Parse query parameters */
    char query[256] = "";
    char limit_str[16] = "10";

    struct mg_str q = mg_http_var(hm->query, mg_str("q"));
    if (q.buf && q.len > 0 && q.len < sizeof(query)) {
        memcpy(query, q.buf, q.len);
        query[q.len] = '\0';
    }

    struct mg_str l = mg_http_var(hm->query, mg_str("limit"));
    if (l.buf && l.len > 0 && l.len < sizeof(limit_str)) {
        memcpy(limit_str, l.buf, l.len);
        limit_str[l.len] = '\0';
    }

    if (strlen(query) == 0) {
        mg_http_reply(c, 400, JSON_CORS_HEADERS,
                      "{\"error\":\"Missing 'q' parameter\"}\n");
        return;
    }

    /* Perform search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = atoi(limit_str);
    if (opts.limit <= 0) opts.limit = 10;
    if (opts.limit > 100) opts.limit = 100;

    LCSearchResult result;
    clock_t start = clock();
    LCStatus status = lc_search(g_index, query, &opts, &result);
    clock_t end = clock();
    double took_ms = (double)(end - start) * 1000.0 / CLOCKS_PER_SEC;

    if (status != LC_OK) {
        mg_http_reply(c, 500, JSON_CORS_HEADERS,
                      "{\"error\":\"Search failed: %s\"}\n", lc_status_string(status));
        return;
    }

    /* Build JSON response */
    char *json = malloc(64 * 1024);  /* 64KB buffer */
    if (!json) {
        lc_search_result_free(&result);
        mg_http_reply(c, 500, JSON_CORS_HEADERS,
                      "{\"error\":\"Out of memory\"}\n");
        return;
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
        const LCEntity *e = lc_search_get_entity(g_index, &result.matches[i]);
        if (!e) continue;

        char escaped_name[512] = "";
        if (e->name) {
            json_escape(e->name, escaped_name, sizeof(escaped_name));
        }

        const char *osm_type = "node";
        if (e->type == LC_ENTITY_WAY) osm_type = "way";
        else if (e->type == LC_ENTITY_RELATION) osm_type = "relation";

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
                          (unsigned long)e->osm_id,
                          osm_type,
                          escaped_name,
                          lc_class_string(e->fclass),
                          e->centroid.lat,
                          e->centroid.lon,
                          result.matches[i].score);
    }

    offset += snprintf(json + offset, 64 * 1024 - offset, "\n  ]\n}\n");

    lc_search_result_free(&result);

    mg_http_reply(c, 200, JSON_CORS_HEADERS, "%s", json);
    free(json);
}

static void handle_autocomplete(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        mg_http_reply(c, 503, JSON_CORS_HEADERS,
                      "{\"error\":\"Index not loaded\"}\n");
        return;
    }

    char query[256] = "";
    char limit_str[16] = "10";

    struct mg_str q = mg_http_var(hm->query, mg_str("q"));
    if (q.buf && q.len > 0 && q.len < sizeof(query)) {
        memcpy(query, q.buf, q.len);
        query[q.len] = '\0';
    }

    struct mg_str l = mg_http_var(hm->query, mg_str("limit"));
    if (l.buf && l.len > 0 && l.len < sizeof(limit_str)) {
        memcpy(limit_str, l.buf, l.len);
        limit_str[l.len] = '\0';
    }

    if (strlen(query) == 0) {
        mg_http_reply(c, 400, JSON_CORS_HEADERS,
                      "{\"error\":\"Missing 'q' parameter\"}\n");
        return;
    }

    int limit = atoi(limit_str);
    if (limit <= 0) limit = 10;
    if (limit > 20) limit = 20;

    LCSearchResult result;
    lc_autocomplete(g_index, query, limit, &result);

    /* Build simple suggestions array */
    char *json = malloc(16 * 1024);
    if (!json) {
        lc_search_result_free(&result);
        mg_http_reply(c, 500, JSON_CORS_HEADERS,
                      "{\"error\":\"Out of memory\"}\n");
        return;
    }

    int offset = snprintf(json, 16 * 1024, "[\n");

    for (size_t i = 0; i < result.num_results; i++) {
        const LCEntity *e = lc_search_get_entity(g_index, &result.matches[i]);
        if (!e || !e->name) continue;

        char escaped[512];
        json_escape(e->name, escaped, sizeof(escaped));

        offset += snprintf(json + offset, 16 * 1024 - offset,
                          "%s  \"%s\"",
                          i > 0 ? ",\n" : "",
                          escaped);
    }

    offset += snprintf(json + offset, 16 * 1024 - offset, "\n]\n");

    lc_search_result_free(&result);

    mg_http_reply(c, 200, JSON_CORS_HEADERS, "%s", json);
    free(json);
}

static void handle_reverse(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        mg_http_reply(c, 503, JSON_CORS_HEADERS,
                      "{\"error\":\"Index not loaded\"}\n");
        return;
    }

    char lat_str[32] = "";
    char lon_str[32] = "";

    struct mg_str lat = mg_http_var(hm->query, mg_str("lat"));
    struct mg_str lon = mg_http_var(hm->query, mg_str("lon"));

    if (lat.buf && lat.len > 0 && lat.len < sizeof(lat_str)) {
        memcpy(lat_str, lat.buf, lat.len);
        lat_str[lat.len] = '\0';
    }
    if (lon.buf && lon.len > 0 && lon.len < sizeof(lon_str)) {
        memcpy(lon_str, lon.buf, lon.len);
        lon_str[lon.len] = '\0';
    }

    if (strlen(lat_str) == 0 || strlen(lon_str) == 0) {
        mg_http_reply(c, 400, JSON_CORS_HEADERS,
                      "{\"error\":\"Missing 'lat' or 'lon' parameter\"}\n");
        return;
    }

    SHCoord coord;
    coord.lat = atof(lat_str);
    coord.lon = atof(lon_str);

    LCReverseResult result;
    LCStatus status = lc_reverse(g_index, coord, NULL, &result);

    if (status != LC_OK) {
        mg_http_reply(c, 500, JSON_CORS_HEADERS,
                      "{\"error\":\"Reverse geocoding failed\"}\n");
        return;
    }

    char address[512] = "";
    lc_format_address(&result, address, sizeof(address));

    char escaped_address[1024];
    json_escape(address, escaped_address, sizeof(escaped_address));

    char *json = malloc(4 * 1024);
    if (!json) {
        lc_reverse_result_free(&result);
        mg_http_reply(c, 500, JSON_CORS_HEADERS,
                      "{\"error\":\"Out of memory\"}\n");
        return;
    }

    int offset = snprintf(json, 4 * 1024,
                          "{\n"
                          "  \"lat\": %.6f,\n"
                          "  \"lon\": %.6f,\n"
                          "  \"display_name\": \"%s\",\n"
                          "  \"distance_m\": %.1f",
                          coord.lat, coord.lon, escaped_address, result.distance_m);

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

    mg_http_reply(c, 200, JSON_CORS_HEADERS, "%s", json);
    free(json);
}

/* ============================================================================
 * Request Router
 * ============================================================================ */

static void handle_request(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Add CORS headers */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204,
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "Access-Control-Allow-Headers: Content-Type\r\n",
                          "");
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/search"), NULL)) {
            handle_search(c, hm);
        } else if (mg_match(hm->uri, mg_str("/api/v1/autocomplete"), NULL)) {
            handle_autocomplete(c, hm);
        } else if (mg_match(hm->uri, mg_str("/api/v1/reverse"), NULL)) {
            handle_reverse(c, hm);
        } else {
            mg_http_reply(c, 404, JSON_CORS_HEADERS,
                          "{\"error\":\"Not found\"}\n");
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options] <pbf-file>\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -p, --port PORT   Listen port (default: 8083)\n");
    fprintf(stderr, "  -h, --help        Show this help\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s -p 8083 data/monaco-latest.osm.pbf\n", prog);
}

int main(int argc, char *argv[]) {
    int port = 8083;
    const char *pbf_file = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            pbf_file = argv[i];
        }
    }

    if (!pbf_file) {
        print_usage(argv[0]);
        return 1;
    }

    /* Load index */
    fprintf(stderr, "locus-api: Loading %s...\n", pbf_file);
    g_index = lc_index_create();
    if (!g_index) {
        fprintf(stderr, "locus-api: Failed to create index\n");
        return 1;
    }

    LCStatus status = lc_index_build_from_pbf(g_index, pbf_file, NULL);
    if (status != LC_OK) {
        fprintf(stderr, "locus-api: Failed to load PBF: %s\n", lc_status_string(status));
        lc_index_free(g_index);
        return 1;
    }

    fprintf(stderr, "locus-api: Loaded %u entities (%.1f MB)\n",
            lc_index_entity_count(g_index),
            (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0));

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start HTTP server */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char listen_addr[64];
    snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%d", port);

    struct mg_connection *c = mg_http_listen(&mgr, listen_addr, handle_request, NULL);
    if (!c) {
        fprintf(stderr, "locus-api: Failed to listen on %s\n", listen_addr);
        lc_index_free(g_index);
        return 1;
    }

    fprintf(stderr, "locus-api: Listening on http://0.0.0.0:%d\n", port);
    fprintf(stderr, "locus-api: Endpoints:\n");
    fprintf(stderr, "  GET /api/v1/health\n");
    fprintf(stderr, "  GET /api/v1/stats\n");
    fprintf(stderr, "  GET /api/v1/search?q=<query>&limit=<n>\n");
    fprintf(stderr, "  GET /api/v1/autocomplete?q=<prefix>&limit=<n>\n");
    fprintf(stderr, "  GET /api/v1/reverse?lat=<lat>&lon=<lon>\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 100);
    }

    fprintf(stderr, "\nlocus-api: Shutting down...\n");
    mg_mgr_free(&mgr);
    lc_index_free(g_index);

    return 0;
}
