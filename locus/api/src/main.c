/*
 * Locus API Server
 *
 * REST API for geocoding operations.
 * Uses mongoose for HTTP serving.
 *
 * Features:
 * - Rate limiting (per-IP token bucket)
 * - Work queue with backpressure
 * - Adaptive capacity tuning
 * - Socket timeout protection
 * - Structured logging
 * - Distributed tracing
 * - Prometheus metrics
 */

#include "locus.h"
#include "lc_serialize.h"
#include "lc_mmap.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

/* Shared library includes */
#include "shared.h"
#include "sh_httpserver.h"
#include "sh_log.h"
#include "sh_trace.h"
#include "sh_metrics.h"
#include "sh_completion.h"
#include "sh_worker_pool.h"
#include "sh_json.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

/*
 * Locus-specific configuration (extends ShServerConfig).
 */
typedef struct {
    /* Common server config (uses sh_args) */
    ShServerConfig server;

    /* Locus-specific */
    char data_file[512];
    char save_path[512];
    int num_workers;  /* Geocode worker threads (0 = auto) */
} LocusServerConfig;

/* Default configuration */
static LocusServerConfig s_config;

/* CORS configuration (uses sh_cors) */
static ShCorsConfig s_cors;

/* ============================================================================
 * Global State
 *
 * DESIGN NOTE: Thread Safety
 *
 * g_index:
 *   - Set once during initialization (before any threads start)
 *   - Read-only after initialization
 *   - All search operations are read-only on the index
 *   - Thread-safe: no concurrent writes
 *
 * s_signo:
 *   - Volatile sig_atomic_t for signal handler communication
 *   - Written only by signal handler
 *   - Read by main event loop
 *   - Thread-safe: atomic access guaranteed by sig_atomic_t
 * ============================================================================ */

static LCIndex *g_index = NULL;
static volatile sig_atomic_t s_signo = 0;

/* Rate limiter instance (uses shared library) */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance (uses shared library) */
static ShWorkQueue *s_work_queue = NULL;

/* Worker pool for geocoding (uses shared library) */
static ShWorkerPool *s_worker_pool = NULL;

/* Adaptive capacity tracker (uses shared library) */
static ShAdaptiveTracker *s_adaptive_tracker = NULL;

/* Geocode work item - passed through the work queue */
typedef enum {
    GEO_TYPE_SEARCH,
    GEO_TYPE_AUTOCOMPLETE,
    GEO_TYPE_REVERSE
} GeoWorkType;

typedef struct {
    /* Request info */
    GeoWorkType type;
    char query[256];
    int limit;
    SHCoord coord;  /* For reverse geocoding */

    /* Response buffer (set by worker) */
    char *response_json;
    size_t response_len;
    int status_code;
    char error_msg[128];

    /* Completion signaling (uses shared library) */
    ShCompletion completion;
} GeoWorkItem;

static void signal_handler(int signo) {
    s_signo = signo;
}

/* JSON building is handled by sh_json.h (ShJsonWriter + ShJsonBuf) */

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

/* Initialize Locus-specific defaults */
static void init_locus_defaults(LocusServerConfig *cfg) {
    /* Initialize common server config using sh_args */
    sh_args_init(&cfg->server);

    /* Override defaults for Locus */
    cfg->server.port = 8083;
    cfg->server.rate_limit_rps = 50.0;      /* Geocoding is fast */
    cfg->server.rate_limit_burst = 100.0;
    cfg->server.work_queue_depth = 128;
    cfg->server.work_queue_timeout = 5.0;

    /* Locus-specific defaults */
    cfg->data_file[0] = '\0';
    cfg->save_path[0] = '\0';
    cfg->num_workers = 0;  /* Auto-detect */
}

/* Load Locus-specific environment variables */
static void load_locus_env(LocusServerConfig *cfg) {
    const char *val;

    /* Load common config using sh_args (handles LOCUS_ prefix) */
    sh_args_load_env(&cfg->server, SH_API_LOCUS);

    /* Locus-specific environment variables */
    if ((val = getenv("LOCUS_DATA_FILE"))) {
        strncpy(cfg->data_file, val, sizeof(cfg->data_file) - 1);
        cfg->data_file[sizeof(cfg->data_file) - 1] = '\0';
    }
    if ((val = getenv("LOCUS_NUM_WORKERS"))) {
        cfg->num_workers = atoi(val);
    }

    /* CORS configuration */
    if ((val = getenv("LOCUS_CORS_ORIGINS"))) {
        sh_cors_parse_origins(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_METHODS"))) {
        sh_cors_set_methods(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_HEADERS"))) {
        sh_cors_set_headers(&s_cors, val);
    }
    if ((val = getenv("LOCUS_CORS_CREDENTIALS"))) {
        s_cors.allow_credentials = (atoi(val) != 0);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

/*
 * Extract origin from request.
 * Thread-safe using thread-local storage for origin buffer.
 */
static const char *get_origin_from_request(struct mg_http_message *hm) {
    struct mg_str *origin_hdr = hm ? mg_http_get_header(hm, "Origin") : NULL;
    if (origin_hdr && origin_hdr->len > 0) {
        static __thread char origin_buf[256];
        size_t len = origin_hdr->len < sizeof(origin_buf) - 1 ?
                     origin_hdr->len : sizeof(origin_buf) - 1;
        memcpy(origin_buf, origin_hdr->buf, len);
        origin_buf[len] = '\0';
        return origin_buf;
    }
    return NULL;
}

/* Get CORS preflight headers for an OPTIONS request */
static void get_cors_preflight_headers(struct mg_http_message *hm, char *buf, size_t size) {
    sh_cors_preflight_headers(&s_cors, get_origin_from_request(hm), buf, size);
}

/* HTTP response helpers - use shared implementation */
static void send_json_cors(struct mg_connection *c, struct mg_http_message *hm,
                           int status, const char *json) {
    sh_mg_reply_json(c, status, &s_cors, get_origin_from_request(hm), json);
}

static void send_error_cors(struct mg_connection *c, struct mg_http_message *hm,
                            int status, const char *message) {
    sh_mg_reply_error(c, status, &s_cors, get_origin_from_request(hm), message);
}

/* ============================================================================
 * Geocode Work Queue Functions
 * ============================================================================ */

/* Initialize a geocode work item */
static void geo_work_item_init(GeoWorkItem *item, GeoWorkType type) {
    memset(item, 0, sizeof(*item));
    item->type = type;
    item->status_code = 500;  /* Default to error */
    sh_completion_init(&item->completion);
}

/* Clean up a geocode work item */
static void geo_work_item_cleanup(GeoWorkItem *item) {
    sh_completion_cleanup(&item->completion);
    free(item->response_json);
    item->response_json = NULL;
}

/* Wait for geocode work item completion with timeout */
static int geo_work_item_wait(GeoWorkItem *item, double timeout_sec) {
    int timeout_ms = (int)(timeout_sec * 1000);
    return sh_completion_wait(&item->completion, timeout_ms);
}

/* Signal that geocode work item is completed */
static void geo_work_item_complete(GeoWorkItem *item) {
    sh_completion_signal(&item->completion);
}

/* Process a search request */
static void process_search(GeoWorkItem *item) {
    if (!g_index) {
        item->status_code = 503;
        snprintf(item->error_msg, sizeof(item->error_msg), "Index not loaded");
        return;
    }

    /* Perform search */
    LCSearchOptions opts;
    lc_search_options_default(&opts);
    opts.limit = item->limit;
    if (opts.limit <= 0) opts.limit = 10;
    if (opts.limit > 100) opts.limit = 100;

    LCSearchResult result;
    struct timeval start, end;
    gettimeofday(&start, NULL);
    LCStatus status = lc_search(g_index, item->query, &opts, &result);
    gettimeofday(&end, NULL);
    double took_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_usec - start.tv_usec) / 1000.0;

    if (status != LC_OK) {
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg),
                 "Search failed: %s", lc_status_string(status));
        return;
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "query");
    sh_json_write_string(&jw, item->query);
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

        if (g_index->mmap_idx) {
            /* v4 mmap path */
            name = lc_mmap_entity_name(g_index->mmap_idx, eid);

            LCEntityType type = lc_mmap_entity_type(g_index->mmap_idx, eid);
            if (type == LC_ENTITY_WAY) osm_type = "way";
            else if (type == LC_ENTITY_RELATION) osm_type = "relation";

            osm_id = lc_mmap_entity_osm_id(g_index->mmap_idx, eid);
            fclass_str = lc_class_string(lc_mmap_entity_fclass(g_index->mmap_idx, eid));
            SHCoord c = lc_mmap_entity_centroid(g_index->mmap_idx, eid);
            lat = c.lat;
            lon = c.lon;
        } else {
            /* Entity store path */
            const LCEntity *e = lc_search_get_entity(g_index, &result.matches[i]);
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
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg), "JSON write error");
        return;
    }

    item->response_json = sh_json_buf_take(&jb);
    item->response_len = jb.len;
    item->status_code = 200;
}

/* Process an autocomplete request */
static void process_autocomplete(GeoWorkItem *item) {
    if (!g_index) {
        item->status_code = 503;
        snprintf(item->error_msg, sizeof(item->error_msg), "Index not loaded");
        return;
    }

    int limit = item->limit;
    if (limit <= 0) limit = 10;
    if (limit > 20) limit = 20;

    LCSearchResult result;
    lc_autocomplete(g_index, item->query, limit, &result);

    /* Build simple suggestions array using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_array_start(&jw);

    for (size_t i = 0; i < result.num_results; i++) {
        const char *name = NULL;

        if (g_index->mmap_idx) {
            name = lc_mmap_entity_name(g_index->mmap_idx, result.matches[i].entity_id);
        } else {
            const LCEntity *e = lc_search_get_entity(g_index, &result.matches[i]);
            if (e) name = e->name;
        }

        if (!name) continue;

        sh_json_write_string(&jw, name);
    }

    sh_json_write_array_end(&jw);

    lc_search_result_free(&result);

    if (jw.error) {
        sh_json_buf_free(&jb);
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg), "JSON write error");
        return;
    }

    item->response_json = sh_json_buf_take(&jb);
    item->response_len = jb.len;
    item->status_code = 200;
}

/* Process a reverse geocode request */
static void process_reverse(GeoWorkItem *item) {
    if (!g_index) {
        item->status_code = 503;
        snprintf(item->error_msg, sizeof(item->error_msg), "Index not loaded");
        return;
    }

    LCReverseResult result;
    LCStatus status = lc_reverse(g_index, item->coord, NULL, &result);

    if (status != LC_OK) {
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg), "Reverse geocoding failed");
        return;
    }

    char address[512] = "";
    lc_format_address(&result, address, sizeof(address));

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "lat");
    sh_json_write_double(&jw, item->coord.lat);
    sh_json_write_key(&jw, "lon");
    sh_json_write_double(&jw, item->coord.lon);
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
        item->status_code = 500;
        snprintf(item->error_msg, sizeof(item->error_msg), "JSON write error");
        return;
    }

    item->response_json = sh_json_buf_take(&jb);
    item->response_len = jb.len;
    item->status_code = 200;
}

/* Geocode worker callback function (called by ShWorkerPool) */
static void geocode_worker_callback(ShWorkItem *queue_item, void *ctx) {
    (void)ctx;

    GeoWorkItem *item = (GeoWorkItem *)queue_item->user_ctx;
    if (!item) {
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Check if request has expired or was cancelled by HTTP handler timeout */
    if (sh_workqueue_item_expired(s_work_queue, queue_item) ||
        sh_completion_is_cancelled(&item->completion)) {
        item->status_code = 504;  /* Gateway Timeout */
        snprintf(item->error_msg, sizeof(item->error_msg), "Request timeout");
        geo_work_item_complete(item);
        sh_workqueue_item_free(queue_item);
        return;
    }

    /* Measure response time for adaptive capacity */
    struct timeval work_start, work_end;
    gettimeofday(&work_start, NULL);

    /* Process based on type */
    switch (item->type) {
        case GEO_TYPE_SEARCH:
            process_search(item);
            break;
        case GEO_TYPE_AUTOCOMPLETE:
            process_autocomplete(item);
            break;
        case GEO_TYPE_REVERSE:
            process_reverse(item);
            break;
    }

    /* Record response time for adaptive capacity */
    gettimeofday(&work_end, NULL);
    double work_ms = (work_end.tv_sec - work_start.tv_sec) * 1000.0 +
                     (work_end.tv_usec - work_start.tv_usec) / 1000.0;

    if (s_adaptive_tracker) {
        sh_adaptive_record(s_adaptive_tracker, work_ms);

        /* Check if rate limiter should be updated */
        ShCapacityParams new_params;
        if (sh_adaptive_update(s_adaptive_tracker, &new_params)) {
            /* Update rate limiter with new parameters */
            if (s_rate_limiter) {
                sh_ratelimit_update_rate(s_rate_limiter,
                                         new_params.rate_limit_rps,
                                         new_params.rate_limit_burst);
            }
        }
    }

    /* Signal completion */
    geo_work_item_complete(item);
    sh_workqueue_item_free(queue_item);
}

/* Submit geocode work via work queue and send response */
static int submit_geocode_work(struct mg_connection *c, struct mg_http_message *hm,
                               GeoWorkItem *item) {
    /* Create queue item */
    ShWorkItem queue_item = {
        .data = NULL,       /* No data to transfer, item is on caller's stack */
        .data_len = 0,
        .user_ctx = item    /* Pass work item as context */
    };

    /* Try to push to queue */
    double pressure;
    if (!sh_workqueue_try_push(s_work_queue, &queue_item, &pressure)) {
        /* Queue is full - backpressure */
        send_error_cors(c, hm, 503, "Server busy, try again later");
        return 0;
    }

    /* Wait for completion with timeout */
    double timeout = s_config.server.work_queue_timeout;
    if (!geo_work_item_wait(item, timeout)) {
        /* Timeout - mark item as cancelled so worker can skip if not started */
        sh_completion_cancel(&item->completion);
        send_error_cors(c, hm, 504, "Request timeout");
        return 0;
    }

    /* Send response based on result */
    if (item->status_code == 200) {
        if (item->response_json && item->response_len > 0) {
            send_json_cors(c, hm, 200, item->response_json);
        } else {
            send_json_cors(c, hm, 200, "{}");
        }
    } else {
        send_error_cors(c, hm, item->status_code, item->error_msg);
    }

    return 1;
}

/* ============================================================================
 * Request Handlers
 * ============================================================================ */

static void handle_health(struct mg_connection *c, struct mg_http_message *hm) {
    sh_mg_handle_health(c, &s_cors, get_origin_from_request(hm),
                        "locus", lc_version());
}

static void handle_stats(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        send_error_cors(c, hm, 503, "Index not loaded");
        return;
    }

    SHBBox bounds = lc_index_bounds(g_index);

    /* Get rate limiter stats */
    ShRateLimitStats rl_stats = {0};
    if (s_rate_limiter) {
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
    }

    /* Get work queue stats */
    ShWorkQueueStats wq_stats = {0};
    if (s_work_queue) {
        sh_workqueue_stats(s_work_queue, &wq_stats);
    }

    /* Get adaptive capacity stats */
    ShAdaptiveStats adaptive_stats = {0};
    if (s_adaptive_tracker) {
        sh_adaptive_stats(s_adaptive_tracker, &adaptive_stats);
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);

    sh_json_write_key(&jw, "entities");
    sh_json_write_int(&jw, lc_index_entity_count(g_index));
    sh_json_write_key(&jw, "memory_mb");
    sh_json_write_double(&jw, (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0));

    /* bounds object */
    sh_json_write_key(&jw, "bounds");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "min_lat");
    sh_json_write_double(&jw, bounds.min_lat);
    sh_json_write_key(&jw, "min_lon");
    sh_json_write_double(&jw, bounds.min_lon);
    sh_json_write_key(&jw, "max_lat");
    sh_json_write_double(&jw, bounds.max_lat);
    sh_json_write_key(&jw, "max_lon");
    sh_json_write_double(&jw, bounds.max_lon);
    sh_json_write_object_end(&jw);

    /* rate_limit object */
    sh_json_write_key(&jw, "rate_limit");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_rate_limiter != NULL);
    sh_json_write_key(&jw, "rps");
    sh_json_write_double(&jw, s_config.server.rate_limit_rps);
    sh_json_write_key(&jw, "burst");
    sh_json_write_double(&jw, s_config.server.rate_limit_burst);
    sh_json_write_key(&jw, "allowed");
    sh_json_write_int(&jw, (int64_t)rl_stats.requests_allowed);
    sh_json_write_key(&jw, "denied");
    sh_json_write_int(&jw, (int64_t)rl_stats.requests_denied);
    sh_json_write_object_end(&jw);

    /* work_queue object */
    sh_json_write_key(&jw, "work_queue");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_work_queue != NULL);
    sh_json_write_key(&jw, "depth");
    sh_json_write_int(&jw, (int64_t)wq_stats.current_depth);
    sh_json_write_key(&jw, "capacity");
    sh_json_write_int(&jw, (int64_t)wq_stats.max_capacity);
    sh_json_write_key(&jw, "pushed");
    sh_json_write_int(&jw, (int64_t)wq_stats.total_pushed);
    sh_json_write_key(&jw, "popped");
    sh_json_write_int(&jw, (int64_t)wq_stats.total_popped);
    sh_json_write_key(&jw, "dropped");
    sh_json_write_int(&jw, (int64_t)wq_stats.total_dropped);
    sh_json_write_key(&jw, "expired");
    sh_json_write_int(&jw, (int64_t)wq_stats.total_expired);
    sh_json_write_object_end(&jw);

    /* adaptive object */
    sh_json_write_key(&jw, "adaptive");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_adaptive_tracker != NULL);
    sh_json_write_key(&jw, "samples");
    sh_json_write_int(&jw, (int64_t)adaptive_stats.sample_count);
    sh_json_write_key(&jw, "response_ms");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "p50");
    sh_json_write_double(&jw, adaptive_stats.p50_ms);
    sh_json_write_key(&jw, "p90");
    sh_json_write_double(&jw, adaptive_stats.p90_ms);
    sh_json_write_key(&jw, "p99");
    sh_json_write_double(&jw, adaptive_stats.p99_ms);
    sh_json_write_object_end(&jw);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);

    char *json = sh_json_buf_take(&jb);
    send_json_cors(c, hm, 200, json);
    free(json);
}

static void handle_search(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        send_error_cors(c, hm, 503, "Index not loaded");
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
        send_error_cors(c, hm, 400, "Missing 'q' parameter");
        return;
    }

    /* Use work queue if enabled */
    if (s_work_queue) {
        GeoWorkItem item;
        geo_work_item_init(&item, GEO_TYPE_SEARCH);
        strncpy(item.query, query, sizeof(item.query) - 1);
        item.query[sizeof(item.query) - 1] = '\0';
        item.limit = atoi(limit_str);

        submit_geocode_work(c, hm, &item);
        geo_work_item_cleanup(&item);
        return;
    }

    /* Fallback: direct execution (work queue disabled) */
    GeoWorkItem item;
    geo_work_item_init(&item, GEO_TYPE_SEARCH);
    strncpy(item.query, query, sizeof(item.query) - 1);
    item.query[sizeof(item.query) - 1] = '\0';
    item.limit = atoi(limit_str);

    process_search(&item);

    if (item.status_code == 200 && item.response_json) {
        send_json_cors(c, hm, 200, item.response_json);
    } else {
        send_error_cors(c, hm, item.status_code, item.error_msg);
    }

    geo_work_item_cleanup(&item);
}

static void handle_autocomplete(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        send_error_cors(c, hm, 503, "Index not loaded");
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
        send_error_cors(c, hm, 400, "Missing 'q' parameter");
        return;
    }

    /* Use work queue if enabled */
    if (s_work_queue) {
        GeoWorkItem item;
        geo_work_item_init(&item, GEO_TYPE_AUTOCOMPLETE);
        strncpy(item.query, query, sizeof(item.query) - 1);
        item.query[sizeof(item.query) - 1] = '\0';
        item.limit = atoi(limit_str);

        submit_geocode_work(c, hm, &item);
        geo_work_item_cleanup(&item);
        return;
    }

    /* Fallback: direct execution */
    GeoWorkItem item;
    geo_work_item_init(&item, GEO_TYPE_AUTOCOMPLETE);
    strncpy(item.query, query, sizeof(item.query) - 1);
    item.query[sizeof(item.query) - 1] = '\0';
    item.limit = atoi(limit_str);

    process_autocomplete(&item);

    if (item.status_code == 200 && item.response_json) {
        send_json_cors(c, hm, 200, item.response_json);
    } else {
        send_error_cors(c, hm, item.status_code, item.error_msg);
    }

    geo_work_item_cleanup(&item);
}

static void handle_reverse(struct mg_connection *c, struct mg_http_message *hm) {
    if (!g_index) {
        send_error_cors(c, hm, 503, "Index not loaded");
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
        send_error_cors(c, hm, 400, "Missing 'lat' or 'lon' parameter");
        return;
    }

    SHCoord coord;
    coord.lat = atof(lat_str);
    coord.lon = atof(lon_str);

    /* Use work queue if enabled */
    if (s_work_queue) {
        GeoWorkItem item;
        geo_work_item_init(&item, GEO_TYPE_REVERSE);
        item.coord = coord;

        submit_geocode_work(c, hm, &item);
        geo_work_item_cleanup(&item);
        return;
    }

    /* Fallback: direct execution */
    GeoWorkItem item;
    geo_work_item_init(&item, GEO_TYPE_REVERSE);
    item.coord = coord;

    process_reverse(&item);

    if (item.status_code == 200 && item.response_json) {
        send_json_cors(c, hm, 200, item.response_json);
    } else {
        send_error_cors(c, hm, item.status_code, item.error_msg);
    }

    geo_work_item_cleanup(&item);
}

/* Handle /metrics endpoint for Prometheus */
static void handle_metrics(struct mg_connection *c) {
    sh_mg_handle_metrics(c);
}

/* ============================================================================
 * Request Router
 * ============================================================================ */

static void handle_request(struct mg_connection *c, int ev, void *ev_data) {
    /* Set socket write timeout on new connections to protect against slow clients */
    if (ev == MG_EV_ACCEPT) {
        sh_mg_set_write_timeout(c, 5000);  /* 5 second write timeout */
        return;
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        ShMetricsTimer req_timer = sh_metrics_timer_start();

        /* Extract or generate trace ID */
        sh_trace_from_headers(sh_mg_trace_header_getter, hm);

        /* Rate limiting check */
        if (!sh_mg_check_rate_limit(c, s_rate_limiter, &s_cors, get_origin_from_request(hm))) {
            SH_LOG_WARN("Rate limit exceeded", "status", "429");
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:429", "endpoint:ratelimit", NULL);
            sh_trace_clear();
            return;  /* 429 already sent */
        }

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            char cors_headers[512];
            get_cors_preflight_headers(hm, cors_headers, sizeof(cors_headers));
            mg_http_reply(c, 204, cors_headers, "");
            sh_trace_clear();
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:health", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c, hm);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:stats", NULL);
        } else if (mg_match(hm->uri, mg_str("/metrics"), NULL)) {
            handle_metrics(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/search"), NULL)) {
            handle_search(c, hm);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                                     "endpoint:search", NULL);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:search", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/autocomplete"), NULL)) {
            handle_autocomplete(c, hm);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                                     "endpoint:autocomplete", NULL);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:autocomplete", NULL);
        } else if (mg_match(hm->uri, mg_str("/api/v1/reverse"), NULL)) {
            handle_reverse(c, hm);
            sh_metrics_timer_observe(req_timer, "http_request_duration_ms",
                                     "endpoint:reverse", NULL);
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:200", "endpoint:reverse", NULL);
        } else {
            send_error_cors(c, hm, 404, "Not found");
            sh_metrics_counter_inc("http_requests_total", 1,
                                   "status:404", "endpoint:unknown", NULL);
        }

        /* Clear trace context at end of request */
        sh_trace_clear();
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Locus Geocoder Server\n\n");
    printf("Usage: %s [options] <pbf-or-idx-file>\n\n", prog);

    /* Common options from sh_args */
    sh_args_usage(prog, "<pbf-or-idx-file>");

    printf("Locus-specific options:\n");
    printf("  -S, --save PATH      Save index to binary file after building\n");
    printf("  --workers N          Geocode worker threads (default: auto)\n");
    printf("\n");
    printf("Locus-specific environment variables:\n");
    printf("  LOCUS_DATA_FILE      Path to data file (PBF or index)\n");
    printf("  LOCUS_NUM_WORKERS    Worker thread count (0 = auto)\n");
    printf("\n");
    printf("CORS configuration:\n");
    printf("  LOCUS_CORS_ORIGINS      Comma-separated allowed origins (empty = allow all)\n");
    printf("  LOCUS_CORS_METHODS      Allowed HTTP methods (default: GET, POST, OPTIONS)\n");
    printf("  LOCUS_CORS_HEADERS      Allowed request headers\n");
    printf("  LOCUS_CORS_CREDENTIALS  Allow credentials (default: 0)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s data/monaco-latest.osm.pbf               # Build from PBF\n", prog);
    printf("  %s -S monaco.idx data/monaco-latest.osm.pbf # Build and save\n", prog);
    printf("  %s monaco.idx                              # Load from binary\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize logging first (reads SH_LOG_LEVEL, SH_LOG_FORMAT from env) */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "locus";
    log_cfg.version = lc_version();
    sh_log_init(&log_cfg);

    /* Initialize defaults */
    init_locus_defaults(&s_config);
    sh_cors_init(&s_cors);

    /* Load config from environment */
    load_locus_env(&s_config);

    /* Parse command line arguments using sh_args for common options */
    int arg_index = sh_args_parse(&s_config.server, argc, argv);
    if (arg_index == -2) {
        /* --help was passed to sh_args */
        print_usage(argv[0]);
        return 0;
    }

    /* Parse Locus-specific arguments */
    for (int i = (arg_index > 0 ? arg_index : 1); i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save") == 0) {
            if (++i < argc) {
                strncpy(s_config.save_path, argv[i], sizeof(s_config.save_path) - 1);
                s_config.save_path[sizeof(s_config.save_path) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (++i < argc) s_config.num_workers = atoi(argv[i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Positional argument - data file */
            strncpy(s_config.data_file, argv[i], sizeof(s_config.data_file) - 1);
            s_config.data_file[sizeof(s_config.data_file) - 1] = '\0';
        }
    }

    /* Validate config */
    if (s_config.data_file[0] == '\0') {
        fprintf(stderr, "Error: No data file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    struct timeval load_start, load_end;
    gettimeofday(&load_start, NULL);

    /* Check if input is a binary index or PBF */
    if (lc_is_binary_index(s_config.data_file)) {
        /* Load via mmap (v4 zero-copy) */
        printf("Loading binary index: %s\n", s_config.data_file);
        g_index = lc_index_mmap(s_config.data_file);
        if (!g_index) {
            fprintf(stderr, "Error: Failed to load binary index\n");
            return 1;
        }
        printf("Index loaded via mmap (fast startup)\n");
    } else {
        /* Build from PBF */
        printf("Building index from: %s\n", s_config.data_file);
        g_index = lc_index_create();
        if (!g_index) {
            fprintf(stderr, "Error: Failed to create index\n");
            return 1;
        }

        LCStatus status = lc_index_build_from_pbf(g_index, s_config.data_file, NULL);
        if (status != LC_OK) {
            fprintf(stderr, "Error: Failed to load PBF: %s\n", lc_status_string(status));
            lc_index_free(g_index);
            return 1;
        }

        /* Save if requested */
        if (s_config.save_path[0] != '\0') {
            printf("Saving to: %s\n", s_config.save_path);
            status = lc_index_save(g_index, s_config.save_path);
            if (status != LC_OK) {
                fprintf(stderr, "Warning: Failed to save index: %s\n", lc_status_string(status));
            } else {
                printf("Saved binary index\n");
            }

            /* Exit if --build-only was specified */
            if (s_config.server.build_only) {
                printf("Build complete (--build-only specified)\n");
                lc_index_free(g_index);
                sh_log_shutdown();
                return 0;
            }
        }
    }

    gettimeofday(&load_end, NULL);
    double load_time = (load_end.tv_sec - load_start.tv_sec) +
                       (load_end.tv_usec - load_start.tv_usec) / 1e6;

    printf("Loaded: %u entities (%.1f MB) in %.3fs\n",
            lc_index_entity_count(g_index),
            (double)lc_index_memory_usage(g_index) / (1024.0 * 1024.0),
            load_time);

    /* Initialize rate limiter (uses shared library) */
    if (s_config.server.rate_limit_enabled) {
        s_rate_limiter = sh_ratelimit_create(s_config.server.rate_limit_rps,
                                             s_config.server.rate_limit_burst, 4096);
        if (s_rate_limiter) {
            printf("Rate limit: %.0f RPS, burst %.0f (IPv4 + IPv6)\n",
                   s_config.server.rate_limit_rps, s_config.server.rate_limit_burst);
        } else {
            fprintf(stderr, "Warning: Failed to create rate limiter\n");
        }
    } else {
        printf("Rate limit: disabled\n");
    }

    /* Initialize work queue and worker pool */
    if (s_config.server.work_queue_enabled) {
        s_work_queue = sh_workqueue_create(s_config.server.work_queue_depth,
                                           s_config.server.work_queue_timeout);
        if (s_work_queue) {
            /* Create worker pool (0 = auto-detect CPU count) */
            ShWorkerPoolConfig pool_cfg = {
                .queue = s_work_queue,
                .callback = geocode_worker_callback,
                .ctx = NULL,
                .poll_timeout_ms = 100
            };
            s_worker_pool = sh_worker_pool_create(s_config.num_workers, &pool_cfg);
            if (s_worker_pool) {
                printf("Work queue: depth %zu, timeout %.1fs, %d workers\n",
                       s_config.server.work_queue_depth, s_config.server.work_queue_timeout,
                       sh_worker_pool_size(s_worker_pool));
            } else {
                fprintf(stderr, "Warning: Failed to create worker pool\n");
                sh_workqueue_free(s_work_queue);
                s_work_queue = NULL;
            }
        } else {
            fprintf(stderr, "Warning: Failed to create work queue\n");
        }
    } else {
        printf("Work queue: disabled\n");
    }

    /* Initialize adaptive capacity tracker */
    if (s_config.server.adaptive_enabled) {
        ShAdaptiveConfig adaptive_cfg;
        sh_adaptive_config_init(&adaptive_cfg);
        int num_workers = s_worker_pool ? sh_worker_pool_size(s_worker_pool) : 4;
        adaptive_cfg.num_workers = num_workers > 0 ? num_workers : 4;
        adaptive_cfg.target_utilization = s_config.server.target_utilization;
        adaptive_cfg.client_timeout_ms = s_config.server.client_timeout_ms;
        adaptive_cfg.burst_tiles = s_config.server.burst_tiles;
        adaptive_cfg.window_size = s_config.server.adaptive_window;
        adaptive_cfg.recalc_interval = s_config.server.adaptive_interval;

        s_adaptive_tracker = sh_adaptive_create(&adaptive_cfg);
        if (s_adaptive_tracker) {
            printf("Adaptive capacity: enabled (window=%zu, interval=%.0f, util=%.0f%%)\n",
                   s_config.server.adaptive_window, s_config.server.adaptive_interval,
                   s_config.server.target_utilization * 100.0);
        } else {
            fprintf(stderr, "Warning: Failed to create adaptive tracker\n");
        }
    } else {
        printf("Adaptive capacity: disabled\n");
    }

    /* Initialize metrics (reads SH_METRICS_STATSD_HOST from env) */
    ShMetricsConfig metrics_cfg = SH_METRICS_CONFIG_DEFAULT;
    metrics_cfg.service = "locus";
    if (sh_metrics_init(&metrics_cfg) == 0) {
        const char *statsd_host = getenv("SH_METRICS_STATSD_HOST");
        if (statsd_host && statsd_host[0]) {
            printf("Metrics: StatsD enabled (%s:%d)\n", statsd_host,
                   metrics_cfg.statsd_port ? metrics_cfg.statsd_port : 8125);
        } else {
            printf("Metrics: Prometheus endpoint at /metrics\n");
        }
    }

    SH_LOG_INFO("Server initializing",
                "data_file", s_config.data_file,
                "port", s_config.server.host);

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start HTTP server */
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char listen_addr[SH_URL_MAX];
    snprintf(listen_addr, sizeof(listen_addr), "http://%s:%d",
             s_config.server.host, s_config.server.port);

    struct mg_connection *conn = mg_http_listen(&mgr, listen_addr, handle_request, NULL);
    if (!conn) {
        fprintf(stderr, "Error: Failed to listen on %s\n", listen_addr);
        /* Cleanup */
        if (s_worker_pool) {
            sh_worker_pool_stop(s_worker_pool);
            sh_worker_pool_join(s_worker_pool);
            sh_worker_pool_free(s_worker_pool);
        }
        sh_workqueue_free(s_work_queue);
        sh_ratelimit_free(s_rate_limiter);
        sh_adaptive_free(s_adaptive_tracker);
        lc_index_free(g_index);
        return 1;
    }

    printf("\nLocus Geocoder Server v%s\n", lc_version());
    printf("Listening on http://%s:%d\n", s_config.server.host, s_config.server.port);
    printf("\nEndpoints:\n");
    printf("  GET  /api/v1/health\n");
    printf("  GET  /api/v1/stats\n");
    printf("  GET  /api/v1/search?q=<query>&limit=<n>\n");
    printf("  GET  /api/v1/autocomplete?q=<prefix>&limit=<n>\n");
    printf("  GET  /api/v1/reverse?lat=<lat>&lon=<lon>\n");
    printf("  GET  /metrics\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Event loop */
    while (s_signo == 0) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("\nShutting down...\n");

    /* Shutdown worker pool first */
    if (s_worker_pool) {
        sh_worker_pool_stop(s_worker_pool);
        sh_worker_pool_join(s_worker_pool);
    }

    /* Print work queue stats */
    if (s_work_queue) {
        ShWorkQueueStats wq_stats;
        sh_workqueue_stats(s_work_queue, &wq_stats);
        printf("Work queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
               (unsigned long)wq_stats.total_pushed,
               (unsigned long)wq_stats.total_popped,
               (unsigned long)wq_stats.total_dropped,
               (unsigned long)wq_stats.total_expired);
    }

    /* Print rate limiter stats */
    if (s_rate_limiter) {
        ShRateLimitStats rl_stats;
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
        printf("Rate limiter: %lu allowed, %lu denied\n",
               (unsigned long)rl_stats.requests_allowed,
               (unsigned long)rl_stats.requests_denied);
    }

    /* Cleanup */
    mg_mgr_free(&mgr);
    sh_worker_pool_free(s_worker_pool);
    sh_workqueue_free(s_work_queue);
    sh_ratelimit_free(s_rate_limiter);
    sh_adaptive_free(s_adaptive_tracker);
    lc_index_free(g_index);

    SH_LOG_INFO("Server shutdown complete");
    sh_metrics_shutdown();
    sh_log_shutdown();

    return 0;
}
