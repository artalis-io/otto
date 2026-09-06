/*
 * Carta Tile Server
 *
 * A lightweight tile server that serves vector (MVT), raster (PNG), and
 * ASCII art tiles from OSM PBF files using carta and Keel.
 *
 * Endpoints:
 *   GET /                         - Tile viewer (served from static dir)
 *   GET /tiles/{z}/{x}/{y}.mvt    - Vector tile (MVT)
 *   GET /tiles/{z}/{x}/{y}.png    - Raster tile (PNG)
 *   GET /tiles/{z}/{x}/{y}.txt    - ASCII art tile
 *   GET /tiles.json               - TileJSON metadata
 *   GET /api/v1/health            - Health check
 *   GET /api/v1/stats             - PBF statistics
 *
 * ASCII tile query parameters:
 *   width=80     - Output width in characters (20-400)
 *   height=0     - Output height in characters (0=auto from aspect)
 *   charset=     - simple, extended (default), blocks, braille
 *   invert=0     - 1 for light background terminals
 *   color=0      - 1 for ANSI 256-color output
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* For strcasecmp */
#include <ctype.h>
#include <unistd.h>   /* For sleep, sysconf */
#include <pthread.h>
#include <sys/time.h> /* For gettimeofday */
#include <errno.h>    /* For ETIMEDOUT */
#include <keel/keel.h>
#include <stddef.h>
#include "carta.h"
#include "ct_api.h"
#include "ct_cache.h"
#include "shared.h"   /* For sh_ratelimit, sh_workqueue */
#include "sh_keelserver.h"  /* Keel-backed sh_kl_* helpers */
#include "sh_completion.h"  /* For ShCompletion */
#include "sh_worker_pool.h" /* For ShWorkerPool */
#include "sh_log.h"         /* For structured logging */
#include "sh_trace.h"       /* For trace ID propagation */
#include "sh_metrics.h"     /* For metrics collection */
#include "sh_json.h"        /* For JSON building */
#include "sh_hash.h"        /* For sh_fnv1a_64 (ETag hashing) */
#include "sh_query.h"       /* For query-string parameter parsing */

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* LOD preset options */
typedef enum {
    LOD_NONE = 0,      /* No LOD filtering */
    LOD_DEFAULT,       /* Default LOD rules */
    LOD_DETAILED,      /* More features at lower zoom */
    LOD_MINIMAL        /* Fewer features (overview) */
} LODPreset;

/* Render quality presets */
typedef enum {
    RENDER_PRESET_DEFAULT = 0, /* Balanced rendering */
    RENDER_PRESET_FAST,        /* Performance-optimized (no casing, labels) */
    RENDER_PRESET_QUALITY      /* Maximum visual quality */
} RenderPreset;

/*
 * Carta-specific configuration (extends ShServerConfig).
 */
typedef struct {
    /* Common server config (uses sh_args) */
    ShServerConfig server;

    /* Carta-specific */
    char pbf_path[512];
    char save_index_path[512];  /* Path to save binary index */
    int min_zoom;
    int max_zoom;
    int tile_size;
    char name[128];
    LODPreset lod_preset;       /* LOD filtering preset */
    RenderPreset render_preset; /* Render quality preset */
    int render_workers;         /* Number of render worker threads (0 = auto) */
    size_t metatile_cache_size; /* Metatile label cache entries (0 = disabled) */
} TileServerConfig;

/* Default configuration */
static TileServerConfig s_config;

/* CORS configuration (uses sh_cors) */
static ShCorsConfig s_cors;

/* Global state */
static CTPBFContext *s_pbf_ctx = NULL;
static CTAPIContext *s_api_ctx = NULL;  /* Transport-agnostic API handler */
static CTLODConfig s_lod_config = {0};
static CTRenderOptions s_render_opts = {0};  /* Render quality options */
static CTTileCache *s_png_cache = NULL;
static CTTileCache *s_mvt_cache = NULL;

/* Cache mutex for thread-safe access */
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Rate limiter instance (uses shared library) */
static ShRateLimiter *s_rate_limiter = NULL;

/* Work queue instance (uses shared library) */
static ShWorkQueue *s_work_queue = NULL;

/* Adaptive capacity tracker (uses shared library) */
static ShAdaptiveTracker *s_adaptive_tracker = NULL;

/* ============================================================================
 * Render Request Context
 *
 * OWNERSHIP / LIFETIME (same rules as Surge, FuelWise and Velo): freed in
 * exactly one place -- done_fn (the item ran) or cancel_fn (dropped at pool
 * shutdown before starting). on_cancel and on_deadline never free, because
 * work_fn may still be running on a worker; they only set `detached`, which
 * is read and written solely on the event loop thread.
 *
 * CONCURRENCY NOTE
 *   The mongoose server ran N event-loop threads, each with its own mg_mgr
 *   listening via SO_REUSEPORT, because every one of them blocked in
 *   render_work_item_wait() for the duration of a render. With KlAsyncOp the
 *   single Keel loop never blocks, so the multi-listener design is no longer
 *   needed: one event loop plus the render pool. --threads now sizes the
 *   render pool.
 * ============================================================================ */

typedef enum {
    RENDER_TYPE_PNG,
    RENDER_TYPE_MVT,
    RENDER_TYPE_ASCII
} RenderType;

typedef struct {
    KlHttpServer *server;
    KlThreadPool *pool;
} AppCtx;

typedef struct {
    KlAsyncOp op;
    AppCtx *app;
    const KlHttpRequest *req;   /* lives inside the conn; valid while suspended */

    /* Request info */
    RenderType type;
    int z, x, y;

    /* ASCII-specific options */
    CTAsciiOptions ascii_opts;

    /* Response buffer (set by the render worker) */
    uint8_t *response_data;
    size_t response_size;
    int status_code;        /* HTTP status code */
    char content_type[64];
    char error_msg[128];

    int detached;
    const char *endpoint;   /* metrics label, static string */
    ShMetricsTimer timer;
} RenderCtx;

/*
 * KlThreadPool exposes no statistics, but /api/v1/stats publishes work-queue
 * counters, so they are tracked here. Every counter is read and written only
 * on the event loop thread.
 */
typedef struct {
    uint64_t pushed;
    uint64_t popped;
    uint64_t dropped;
    uint64_t expired;
} CartaQueueStats;

static KlThreadPool *s_pool = NULL;
static CartaQueueStats s_qstats;
static int s_num_workers = 0;
static char s_listen_url[SH_URL_MAX] = "";

/* Progress callback for PBF loading */
static void pbf_progress_callback(const char *phase, size_t current,
                                   size_t total, void *user_data)
{
    (void)user_data;
    if (total > 0) {
        int pct = (int)(100 * current / total);
        printf("\r%s: %d%% (%zu / %zu bytes)", phase, pct, current, total);
        fflush(stdout);
    } else {
        printf("\r%s: %zu", phase, current);
        fflush(stdout);
    }
}

/* Forward declarations */

/* ============================================================================
 * Render Context Helpers
 * ============================================================================ */

/* Defined with the other response helpers below. */
static void send_error_cors(KlHttpResponse *res, const KlHttpRequest *req,
                            int status, const char *message);
static void send_tile_cors(KlHttpResponse *res, const KlHttpRequest *req,
                           const char *content_type, const uint8_t *data,
                           size_t size);

static void render_ctx_free(RenderCtx *ctx) {
    if (!ctx) return;
    free(ctx->response_data);
    free(ctx);
}

static void record_metrics(ShMetricsTimer timer, const char *endpoint) {
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:200", endpoint, NULL);
    sh_metrics_timer_observe(timer, "http_request_duration_ms", endpoint, NULL);
}

static void process_png_render(RenderCtx *item)
{
    int z = item->z, x = item->x, y = item->y;

    /* Check cache first */
    if (s_png_cache) {
        const uint8_t *cached_data;
        size_t cached_size;
        pthread_mutex_lock(&s_cache_mutex);
        int hit = ct_cache_get(s_png_cache, z, x, y, &cached_data, &cached_size);
        if (hit) {
            item->response_data = malloc(cached_size);
            if (item->response_data) {
                memcpy(item->response_data, cached_data, cached_size);
                item->response_size = cached_size;
                item->status_code = 200;
                strncpy(item->content_type, "image/png", sizeof(item->content_type) - 1);
                item->content_type[sizeof(item->content_type) - 1] = '\0';
            }
            pthread_mutex_unlock(&s_cache_mutex);
            if (item->response_data) return;
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    /* Use transport-agnostic API to generate tile */
    size_t size;
    item->response_data = ct_api_generate_png(s_api_ctx, z, x, y, &size);

    if (!item->response_data || size == 0) {
        if (item->response_data) free(item->response_data);
        item->response_data = NULL;
        item->status_code = 500;
        strncpy(item->error_msg, "Tile generation failed",
                sizeof(item->error_msg) - 1);
        item->error_msg[sizeof(item->error_msg) - 1] = '\0';
        return;
    }

    item->response_size = size;
    item->status_code = 200;
    strncpy(item->content_type, "image/png", sizeof(item->content_type) - 1);
    item->content_type[sizeof(item->content_type) - 1] = '\0';

    /* Cache the result */
    if (s_png_cache) {
        pthread_mutex_lock(&s_cache_mutex);
        ct_cache_put(s_png_cache, z, x, y, item->response_data, size);
        pthread_mutex_unlock(&s_cache_mutex);
    }
}

/* Process an MVT tile render request */
static void process_mvt_render(RenderCtx *item)
{
    int z = item->z, x = item->x, y = item->y;

    /* Check cache first */
    if (s_mvt_cache) {
        const uint8_t *cached_data;
        size_t cached_size;
        pthread_mutex_lock(&s_cache_mutex);
        int hit = ct_cache_get(s_mvt_cache, z, x, y, &cached_data, &cached_size);
        if (hit) {
            item->response_data = malloc(cached_size);
            if (item->response_data) {
                memcpy(item->response_data, cached_data, cached_size);
                item->response_size = cached_size;
                item->status_code = 200;
                strncpy(item->content_type, "application/vnd.mapbox-vector-tile",
                        sizeof(item->content_type) - 1);
                item->content_type[sizeof(item->content_type) - 1] = '\0';
            }
            pthread_mutex_unlock(&s_cache_mutex);
            if (item->response_data) return;
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    /* Use transport-agnostic API to generate tile */
    size_t size;
    item->response_data = ct_api_generate_mvt(s_api_ctx, z, x, y, &size);

    if (!item->response_data) {
        item->status_code = 500;
        strncpy(item->error_msg, "Tile generation failed",
                sizeof(item->error_msg) - 1);
        item->error_msg[sizeof(item->error_msg) - 1] = '\0';
        return;
    }

    item->response_size = size;
    item->status_code = 200;
    strncpy(item->content_type, "application/vnd.mapbox-vector-tile",
            sizeof(item->content_type) - 1);
    item->content_type[sizeof(item->content_type) - 1] = '\0';

    /* Cache the result */
    if (s_mvt_cache) {
        pthread_mutex_lock(&s_cache_mutex);
        ct_cache_put(s_mvt_cache, z, x, y, item->response_data, size);
        pthread_mutex_unlock(&s_cache_mutex);
    }
}

/* Process an ASCII tile render request */
static void process_ascii_render(RenderCtx *item)
{
    int z = item->z, x = item->x, y = item->y;

    /* Build query string from parsed ASCII options */
    CTAsciiOptions *opts = &item->ascii_opts;
    const char *charset_str = "extended";
    switch (opts->charset) {
        case CT_ASCII_SIMPLE:   charset_str = "simple"; break;
        case CT_ASCII_EXTENDED: charset_str = "extended"; break;
        case CT_ASCII_BLOCKS:   charset_str = "blocks"; break;
        case CT_ASCII_BRAILLE:  charset_str = "braille"; break;
    }

    char query[256];
    snprintf(query, sizeof(query), "width=%d&height=%d&charset=%s&invert=%d&color=%d",
             opts->width, opts->height, charset_str, opts->invert, opts->color);

    /* Use transport-agnostic API to generate ASCII tile */
    size_t size;
    item->response_data = (uint8_t *)ct_api_generate_ascii(s_api_ctx, z, x, y,
                                                            query, &size);

    if (!item->response_data || size == 0) {
        if (item->response_data) free(item->response_data);
        item->response_data = NULL;
        item->status_code = 500;
        strncpy(item->error_msg, "ASCII tile generation failed",
                sizeof(item->error_msg) - 1);
        item->error_msg[sizeof(item->error_msg) - 1] = '\0';
        return;
    }

    item->response_size = size;
    item->status_code = 200;
    strncpy(item->content_type, "text/plain; charset=utf-8",
            sizeof(item->content_type) - 1);
    item->content_type[sizeof(item->content_type) - 1] = '\0';
}

/* Render worker callback function (called by ShWorkerPool) */
/* Worker thread: render the tile. Touches only this context. */
static void render_work_fn(void *user_data)
{
    RenderCtx *item = (RenderCtx *)user_data;

    /* Measure render time for adaptive capacity */
    struct timeval render_start, render_end;
    gettimeofday(&render_start, NULL);

    switch (item->type) {
        case RENDER_TYPE_PNG:
            process_png_render(item);
            break;
        case RENDER_TYPE_MVT:
            process_mvt_render(item);
            break;
        case RENDER_TYPE_ASCII:
            process_ascii_render(item);
            break;
    }

    gettimeofday(&render_end, NULL);
    double render_ms = (render_end.tv_sec - render_start.tv_sec) * 1000.0 +
                       (render_end.tv_usec - render_start.tv_usec) / 1000.0;

    if (s_adaptive_tracker) {
        sh_adaptive_record(s_adaptive_tracker, render_ms);

        ShCapacityParams new_params;
        if (sh_adaptive_update(s_adaptive_tracker, &new_params)) {
            if (s_rate_limiter) {
                sh_ratelimit_update_rate(s_rate_limiter,
                                         new_params.rate_limit_rps,
                                         new_params.rate_limit_burst);
            }
        }
    }
}

/* Write whatever the worker produced. Shared by done_fn and the inline path. */
static void render_reply(KlHttpResponse *res, const KlHttpRequest *req,
                         RenderCtx *item)
{
    if (item->status_code == 200) {
        if (item->response_data && item->response_size > 0) {
            send_tile_cors(res, req, item->content_type, item->response_data,
                           item->response_size);
        } else {
            /* Empty tile: still a valid response */
            send_tile_cors(res, req, item->content_type, NULL, 0);
        }
    } else {
        send_error_cors(res, req, item->status_code,
                        item->error_msg[0] ? item->error_msg : "Render failed");
    }
}

/* Event loop thread: write the response and resume the connection. */
static void render_done_fn(void *user_data)
{
    RenderCtx *ctx = (RenderCtx *)user_data;

    s_qstats.popped++;

    if (ctx->detached) {
        render_ctx_free(ctx);
        return;
    }

    render_reply(kl_http_conn_response(ctx->op.conn), ctx->req, ctx);
    record_metrics(ctx->timer, ctx->endpoint);

    kl_async_complete(ctx->app->server, &ctx->op);
    render_ctx_free(ctx);
}

/* Pool shutdown dropped the item before it started. */
static void render_cancel_fn(void *user_data)
{
    render_ctx_free((RenderCtx *)user_data);
}

/*
 * Declare the send. kl_async_complete() re-arms the fd but leaves the
 * connection SUSPENDED unless on_resume says what happens next; without this
 * the response is never written and the client hangs. Keel's
 * examples/thread_pool and examples/async_thread_pool leave this a no-op and
 * hang for exactly that reason; tests/smoke_iouring_async.c is the correct
 * reference, and kl_http_request_send_response() is its public equivalent.
 */
static void render_on_resume(KlAsyncOp *op, void *ud)
{
    (void)ud;
    RenderCtx *ctx = (RenderCtx *)((char *)op - offsetof(RenderCtx, op));
    kl_http_request_send_response(ctx->req);
}

/* Connection died while suspended; the worker may still be running. */
static void render_on_cancel(KlAsyncOp *op, void *ud)
{
    (void)ud;
    ((RenderCtx *)((char *)op - offsetof(RenderCtx, op)))->detached = 1;
}

/* Deadline exceeded: reply 504 now, let done_fn free the context later. */
static void render_on_deadline(KlAsyncOp *op, void *ud)
{
    (void)ud;
    RenderCtx *ctx = (RenderCtx *)((char *)op - offsetof(RenderCtx, op));

    if (ctx->detached) return;
    ctx->detached = 1;
    s_qstats.expired++;

    send_error_cors(kl_http_conn_response(op->conn), ctx->req, 504,
                    "Request timeout");
    record_metrics(ctx->timer, ctx->endpoint);

    kl_async_complete(ctx->app->server, op);
}

/* ============================================================================
 * Configuration Loading
 * ============================================================================ */

/* Forward declaration */
static LODPreset parse_lod_preset(const char *str);

/* Trim whitespace from string */
static char *trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

/* Load configuration from YAML-like file */
static int load_config_file(const char *filename, TileServerConfig *cfg) {
    FILE *f = fopen(filename, "r");
    if (!f) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (*trimmed == '#' || *trimmed == '\0') continue;

        char *colon = strchr(trimmed, ':');
        if (!colon) continue;

        *colon = '\0';
        char *key = trim(trimmed);
        char *value = trim(colon + 1);

        /* Remove quotes from value */
        size_t vlen = strlen(value);
        if (vlen >= 2 && ((value[0] == '"' && value[vlen-1] == '"') ||
                          (value[0] == '\'' && value[vlen-1] == '\''))) {
            value[vlen-1] = '\0';
            value++;
        }

        if (strcmp(key, "pbf_path") == 0 || strcmp(key, "pbf") == 0) {
            strncpy(cfg->pbf_path, value, sizeof(cfg->pbf_path) - 1);
            cfg->pbf_path[sizeof(cfg->pbf_path) - 1] = '\0';
        } else if (strcmp(key, "static_dir") == 0 || strcmp(key, "static") == 0) {
            strncpy(cfg->server.static_dir, value, sizeof(cfg->server.static_dir) - 1);
            cfg->server.static_dir[sizeof(cfg->server.static_dir) - 1] = '\0';
        } else if (strcmp(key, "listen") == 0 || strcmp(key, "host") == 0) {
            strncpy(cfg->server.host, value, sizeof(cfg->server.host) - 1);
            cfg->server.host[sizeof(cfg->server.host) - 1] = '\0';
        } else if (strcmp(key, "port") == 0) {
            cfg->server.port = sh_parse_int(value, cfg->server.port, 1, 65535);
        } else if (strcmp(key, "min_zoom") == 0) {
            cfg->min_zoom = sh_parse_int(value, cfg->min_zoom, 0, 22);
        } else if (strcmp(key, "max_zoom") == 0) {
            cfg->max_zoom = sh_parse_int(value, cfg->max_zoom, 0, 22);
        } else if (strcmp(key, "tile_size") == 0) {
            cfg->tile_size = sh_parse_int(value, cfg->tile_size, 64, 4096);
        } else if (strcmp(key, "name") == 0) {
            strncpy(cfg->name, value, sizeof(cfg->name) - 1);
            cfg->name[sizeof(cfg->name) - 1] = '\0';
        } else if (strcmp(key, "lod") == 0) {
            cfg->lod_preset = parse_lod_preset(value);
        }
    }

    fclose(f);
    return 0;
}

/* Parse LOD preset from string */
static LODPreset parse_lod_preset(const char *str) {
    if (strcasecmp(str, "none") == 0 || strcasecmp(str, "off") == 0 ||
        strcasecmp(str, "disabled") == 0 || strcmp(str, "0") == 0) {
        return LOD_NONE;
    }
    if (strcasecmp(str, "default") == 0 || strcasecmp(str, "on") == 0 ||
        strcmp(str, "1") == 0) {
        return LOD_DEFAULT;
    }
    if (strcasecmp(str, "detailed") == 0) {
        return LOD_DETAILED;
    }
    if (strcasecmp(str, "minimal") == 0) {
        return LOD_MINIMAL;
    }
    return LOD_DEFAULT;  /* Default if unrecognized */
}

/* Parse render preset from string */
static RenderPreset parse_render_preset(const char *str) {
    if (strcasecmp(str, "fast") == 0) {
        return RENDER_PRESET_FAST;
    }
    if (strcasecmp(str, "quality") == 0) {
        return RENDER_PRESET_QUALITY;
    }
    return RENDER_PRESET_DEFAULT;  /* Default if unrecognized */
}

/* Initialize Carta-specific defaults */
static void init_carta_defaults(TileServerConfig *cfg) {
    /* Initialize common server config using sh_args */
    sh_args_init(&cfg->server);

    /* Override defaults for Carta */
    cfg->server.port = 8081;
    cfg->server.work_queue_depth = 256;
    cfg->server.work_queue_timeout = 5.0;

    /* Carta-specific defaults */
    cfg->pbf_path[0] = '\0';
    cfg->save_index_path[0] = '\0';
    cfg->min_zoom = 0;
    cfg->max_zoom = 18;
    cfg->tile_size = 512;
    strncpy(cfg->name, "Carta Tile Server", sizeof(cfg->name) - 1);
    cfg->name[sizeof(cfg->name) - 1] = '\0';
    cfg->lod_preset = LOD_DEFAULT;
    cfg->render_preset = RENDER_PRESET_DEFAULT;
    cfg->render_workers = 0;  /* Auto-detect */
    cfg->metatile_cache_size = CT_METATILE_CACHE_DEFAULT;
}

/* Load Carta-specific environment variables */
static void load_carta_env(TileServerConfig *cfg) {
    const char *val;

    /* Load common config using sh_args (handles CARTA_ prefix) */
    sh_args_load_env(&cfg->server, SH_API_CARTA);

    /* Carta-specific environment variables */
    if ((val = getenv("TILE_PBF_PATH")) || (val = getenv("PBF_PATH")) ||
        (val = getenv("CARTA_DATA_FILE"))) {
        strncpy(cfg->pbf_path, val, sizeof(cfg->pbf_path) - 1);
        cfg->pbf_path[sizeof(cfg->pbf_path) - 1] = '\0';
    }
    if ((val = getenv("TILE_MIN_ZOOM")) || (val = getenv("CARTA_MIN_ZOOM"))) {
        cfg->min_zoom = sh_parse_int(val, cfg->min_zoom, 0, 22);
    }
    if ((val = getenv("TILE_MAX_ZOOM")) || (val = getenv("CARTA_MAX_ZOOM"))) {
        cfg->max_zoom = sh_parse_int(val, cfg->max_zoom, 0, 22);
    }
    if ((val = getenv("TILE_SIZE")) || (val = getenv("CARTA_TILE_SIZE"))) {
        cfg->tile_size = sh_parse_int(val, cfg->tile_size, 64, 4096);
    }
    if ((val = getenv("TILE_NAME")) || (val = getenv("CARTA_NAME"))) {
        strncpy(cfg->name, val, sizeof(cfg->name) - 1);
        cfg->name[sizeof(cfg->name) - 1] = '\0';
    }
    if ((val = getenv("TILE_LOD")) || (val = getenv("CARTA_LOD"))) {
        cfg->lod_preset = parse_lod_preset(val);
    }
    if ((val = getenv("CARTA_RENDER_PRESET"))) {
        cfg->render_preset = parse_render_preset(val);
    }
    if ((val = getenv("CARTA_RENDER_WORKERS"))) {
        cfg->render_workers = sh_parse_int(val, cfg->render_workers, 0, 256);
    }
    if ((val = getenv("CARTA_METATILE_CACHE"))) {
        cfg->metatile_cache_size = (size_t)sh_parse_int(val, (int)cfg->metatile_cache_size, 0, 100000);
    }

    /* CORS configuration */
    if ((val = getenv("CARTA_CORS_ORIGINS"))) {
        sh_cors_parse_origins(&s_cors, val);
    }
    if ((val = getenv("CARTA_CORS_METHODS"))) {
        sh_cors_set_methods(&s_cors, val);
    }
    if ((val = getenv("CARTA_CORS_HEADERS"))) {
        sh_cors_set_headers(&s_cors, val);
    }
    if ((val = getenv("CARTA_CORS_CREDENTIALS"))) {
        s_cors.allow_credentials = (sh_parse_int(val, 0, 0, 1) != 0);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

/*
 * Extract origin from request.
 * Thread-safe using thread-local storage for origin buffer.
 */
static const char *get_origin_from_request(const KlHttpRequest *req) {
    return sh_kl_origin(req);
}

/* HTTP response helpers - use shared implementation */
static void send_json_cors(KlHttpResponse *res, const KlHttpRequest *req,
                           int status, const char *json) {
    sh_kl_reply_json(res, status, &s_cors, get_origin_from_request(req), json);
}

static void send_error_cors(KlHttpResponse *res, const KlHttpRequest *req,
                            int status, const char *message) {
    sh_kl_reply_error(res, status, &s_cors, get_origin_from_request(req), message);
}

/*
 * send_tile_cors sends binary tile data with ETag support.
 * When req is non-NULL, honours If-None-Match with a conditional 304.
 */
static void send_tile_cors(KlHttpResponse *res, const KlHttpRequest *req,
                           const char *content_type, const uint8_t *data, size_t size) {
    /* Compute ETag from tile bytes */
    char etag[24];
    if (data && size > 0) {
        uint64_t hash = sh_fnv1a_64(data, size);
        snprintf(etag, sizeof(etag), "\"%016llx\"", (unsigned long long)hash);
    } else {
        etag[0] = '\0';
    }

    /* Conditional request: If-None-Match */
    if (req && etag[0]) {
        const char *inm = kl_http_request_header(req, "If-None-Match");
        if (inm && strcmp(inm, etag) == 0) {
            kl_http_response_status(res, 304);
            kl_http_response_header(res, "ETag", etag);
            kl_http_response_header(res, "Cache-Control", "public, max-age=86400");
            sh_kl_apply_cors(res, &s_cors, get_origin_from_request(req));
            kl_http_response_body_borrow(res, "", 0);
            return;
        }
    }

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", content_type);
    kl_http_response_header(res, "Cache-Control", "public, max-age=86400");
    if (etag[0]) kl_http_response_header(res, "ETag", etag);
    sh_kl_apply_cors(res, &s_cors, get_origin_from_request(req));
    /* Copy: Keel body setters borrow, and callers free their buffers. */
    kl_http_response_body_copy(res, (const char *)data, size);
}

/* Legacy wrappers without request context (used by internal callbacks) */
static void send_error(KlHttpResponse *res, int status, const char *message) {
    send_error_cors(res, NULL, status, message);
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

/* GET /api/v1/health - uses shared helper */
static void handle_health(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    record_metrics(timer, "endpoint:health");
    sh_kl_handle_health(res, &s_cors, get_origin_from_request(req),
                        "carta-tile-server", ct_version());
}

/* GET /api/v1/stats */
static void handle_stats(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    if (!s_pbf_ctx) {
        send_error_cors(res, req, 503, "PBF not loaded");
        return;
    }

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(s_pbf_ctx, &nodes, &ways, &features, &bbox);

    /* Get work queue stats */
    ShWorkQueueStats wq_stats = {0};
    if (s_work_queue) {
        sh_workqueue_stats(s_work_queue, &wq_stats);
    }

    /* Get rate limiter stats */
    ShRateLimitStats rl_stats = {0};
    if (s_rate_limiter) {
        sh_ratelimit_stats(s_rate_limiter, &rl_stats);
    }

    /* Get cache stats */
    size_t png_entries = 0, png_bytes = 0;
    uint64_t png_hits = 0, png_misses = 0;
    size_t mvt_entries = 0, mvt_bytes = 0;
    uint64_t mvt_hits = 0, mvt_misses = 0;

    if (s_png_cache) {
        ct_cache_stats(s_png_cache, &png_entries, &png_bytes, &png_hits, &png_misses);
    }
    if (s_mvt_cache) {
        ct_cache_stats(s_mvt_cache, &mvt_entries, &mvt_bytes, &mvt_hits, &mvt_misses);
    }

    /* Get adaptive capacity stats */
    ShAdaptiveStats adaptive_stats = {0};
    ShCapacityParams adaptive_params = {0};
    int has_adaptive_params = 0;
    if (s_adaptive_tracker) {
        sh_adaptive_stats(s_adaptive_tracker, &adaptive_stats);
        has_adaptive_params = sh_adaptive_get_params(s_adaptive_tracker, &adaptive_params);
    }

    /* Build JSON response using streaming writer */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter jw;
    sh_json_writer_init(&jw, sh_json_buf_write, &jb);

    sh_json_write_object_start(&jw);

    /* pbf object */
    sh_json_write_key(&jw, "pbf");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "path");
    sh_json_write_string(&jw, s_config.pbf_path);
    sh_json_write_key(&jw, "nodes");
    sh_json_write_int(&jw, (int64_t)nodes);
    sh_json_write_key(&jw, "ways");
    sh_json_write_int(&jw, (int64_t)ways);
    sh_json_write_key(&jw, "features");
    sh_json_write_int(&jw, (int64_t)features);
    sh_json_write_key(&jw, "bbox");
    sh_json_write_array_start(&jw);
    sh_json_write_double(&jw, bbox.min_lon);
    sh_json_write_double(&jw, bbox.min_lat);
    sh_json_write_double(&jw, bbox.max_lon);
    sh_json_write_double(&jw, bbox.max_lat);
    sh_json_write_array_end(&jw);
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

    /* adaptive object */
    sh_json_write_key(&jw, "adaptive");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "enabled");
    sh_json_write_bool(&jw, s_adaptive_tracker != NULL);
    sh_json_write_key(&jw, "samples");
    sh_json_write_int(&jw, (int64_t)adaptive_stats.sample_count);
    sh_json_write_key(&jw, "recalculations");
    sh_json_write_int(&jw, (int64_t)adaptive_stats.recalc_count);
    sh_json_write_key(&jw, "response_ms");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "p50");
    sh_json_write_double(&jw, adaptive_stats.p50_ms);
    sh_json_write_key(&jw, "p90");
    sh_json_write_double(&jw, adaptive_stats.p90_ms);
    sh_json_write_key(&jw, "p99");
    sh_json_write_double(&jw, adaptive_stats.p99_ms);
    sh_json_write_key(&jw, "avg");
    sh_json_write_double(&jw, adaptive_stats.avg_ms);
    sh_json_write_key(&jw, "ema");
    sh_json_write_double(&jw, adaptive_stats.ema_ms);
    sh_json_write_object_end(&jw);
    sh_json_write_key(&jw, "current");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "rate_limit_rps");
    sh_json_write_double(&jw, has_adaptive_params ? adaptive_params.rate_limit_rps : 0.0);
    sh_json_write_key(&jw, "queue_depth");
    sh_json_write_int(&jw, (int64_t)(has_adaptive_params ? adaptive_params.queue_depth : 0));
    sh_json_write_key(&jw, "throughput_rps");
    sh_json_write_double(&jw, has_adaptive_params ? adaptive_params.max_throughput_rps : 0.0);
    sh_json_write_object_end(&jw);
    sh_json_write_object_end(&jw);

    /* cache object */
    sh_json_write_key(&jw, "cache");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "png");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "entries");
    sh_json_write_int(&jw, (int64_t)png_entries);
    sh_json_write_key(&jw, "bytes");
    sh_json_write_int(&jw, (int64_t)png_bytes);
    sh_json_write_key(&jw, "hits");
    sh_json_write_int(&jw, (int64_t)png_hits);
    sh_json_write_key(&jw, "misses");
    sh_json_write_int(&jw, (int64_t)png_misses);
    sh_json_write_object_end(&jw);
    sh_json_write_key(&jw, "mvt");
    sh_json_write_object_start(&jw);
    sh_json_write_key(&jw, "entries");
    sh_json_write_int(&jw, (int64_t)mvt_entries);
    sh_json_write_key(&jw, "bytes");
    sh_json_write_int(&jw, (int64_t)mvt_bytes);
    sh_json_write_key(&jw, "hits");
    sh_json_write_int(&jw, (int64_t)mvt_hits);
    sh_json_write_key(&jw, "misses");
    sh_json_write_int(&jw, (int64_t)mvt_misses);
    sh_json_write_object_end(&jw);
    sh_json_write_object_end(&jw);

    sh_json_write_object_end(&jw);

    char *json = sh_json_buf_take(&jb);
    record_metrics(timer, "endpoint:stats");
    send_json_cors(res, req, 200, json);
    free(json);
}

/* GET /tiles.json - TileJSON metadata */
static void handle_tilejson(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    ShMetricsTimer timer = sh_metrics_timer_start();
    if (!s_api_ctx) {
        send_error_cors(res, req, 503, "API not initialized");
        return;
    }

    /* Get host header for building tile URLs */
    (void)timer;  /* recorded at the send site below */
    const char *host_hdr = kl_http_request_header(req, "Host");
    char host_buf[256] = "localhost:8081";
    if (host_hdr && host_hdr[0]) {
        snprintf(host_buf, sizeof(host_buf), "%s", host_hdr);
    }

    /* Use transport-agnostic API to generate TileJSON */
    size_t len;
    char *response = ct_api_generate_tilejson(s_api_ctx, host_buf, &len);
    if (!response) {
        send_error_cors(res, req, 500, "TileJSON generation failed");
        return;
    }

    send_json_cors(res, req, 200, response);
    free(response);
}

/* Submit render work via work queue and send response */
/*
 * Dispatch a render. Takes ownership of ctx in every path.
 * Renders inline when the queue is disabled, matching the old behaviour.
 */
static void submit_render_work(KlHttpRequest *req, KlHttpResponse *res,
                               void *ud, RenderCtx *ctx)
{
    AppCtx *app = (AppCtx *)ud;

    if (!s_pool) {
        render_work_fn(ctx);
        render_reply(res, req, ctx);
        record_metrics(ctx->timer, ctx->endpoint);
        render_ctx_free(ctx);
        return;
    }

    ctx->app = app;
    ctx->req = req;
    ctx->op.on_resume = render_on_resume;
    ctx->op.on_cancel = render_on_cancel;
    ctx->op.on_deadline = render_on_deadline;
    if (s_config.server.work_queue_timeout > 0.0) {
        ctx->op.deadline_ms = kl_monotonic_ms() +
            (uint64_t)(s_config.server.work_queue_timeout * 1000.0);
    }

    if (kl_async_suspend(app->server, kl_http_request_conn(req), &ctx->op) < 0) {
        render_ctx_free(ctx);
        send_error_cors(res, req, 500, "Failed to suspend request");
        return;
    }

    KlWorkItem item = {
        .work_fn   = render_work_fn,
        .done_fn   = render_done_fn,
        .cancel_fn = render_cancel_fn,
        .user_data = ctx,
    };

    if (kl_thread_pool_submit(app->pool, &item) < 0) {
        /* Queue full - backpressure, same 503 as the old work queue. */
        s_qstats.dropped++;
        ctx->detached = 1;
        send_error_cors(res, req, 503, "Server busy, try again later");
        record_metrics(ctx->timer, ctx->endpoint);
        kl_async_complete(app->server, &ctx->op);
        render_ctx_free(ctx);
        return;
    }

    s_qstats.pushed++;
}

/* Allocate a render context with the common fields filled in. */
static RenderCtx *render_ctx_new(RenderType type, int z, int x, int y,
                                 const char *endpoint, ShMetricsTimer timer)
{
    RenderCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->type = type;
    ctx->z = z;
    ctx->x = x;
    ctx->y = y;
    ctx->status_code = 500;
    ctx->endpoint = endpoint;
    ctx->timer = timer;
    return ctx;
}

/* GET /tiles/{z}/{x}/{y}.mvt */
static void handle_mvt_tile(KlHttpRequest *req, KlHttpResponse *res, void *ud,
                            int z, int x, int y, ShMetricsTimer timer) {
    if (!s_pbf_ctx) {
        send_error(res, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(res, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(res, 400, "Tile coordinates out of range");
        return;
    }

    /* Check cache first (thread-safe) - before queueing */
    if (s_mvt_cache) {
        const uint8_t *cached_data;
        size_t cached_size;
        pthread_mutex_lock(&s_cache_mutex);
        int hit = ct_cache_get(s_mvt_cache, z, x, y, &cached_data, &cached_size);
        if (hit) {
            /* Copy data before unlocking - cache data may be evicted */
            uint8_t *copy = malloc(cached_size);
            if (copy) {
                memcpy(copy, cached_data, cached_size);
                pthread_mutex_unlock(&s_cache_mutex);
                send_tile_cors(res, req, "application/vnd.mapbox-vector-tile", copy, cached_size);
                free(copy);
                return;
            }
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    RenderCtx *ctx = render_ctx_new(RENDER_TYPE_MVT, z, x, y, "endpoint:mvt", timer);
    if (!ctx) {
        send_error_cors(res, req, 500, "Memory allocation failed");
        return;
    }
    submit_render_work(req, res, ud, ctx);
}

/* Parse query string for a parameter with bounds, returns default if not found/invalid.
 * Thin wrapper over sh_query_get_int_bounded(). */
static int get_query_int(const char *query, const char *name, int default_val,
                         int min_val, int max_val) {
    return sh_query_get_int_bounded(query ? query : "", name,
                                    default_val, min_val, max_val);
}

/* GET /tiles/{z}/{x}/{y}.txt or .ascii */
static void handle_ascii_tile(KlHttpRequest *req, KlHttpResponse *res, void *ud,
                              int z, int x, int y, ShMetricsTimer timer) {
    char query[512];
    size_t qlen = req->query_len < sizeof(query) - 1 ? req->query_len
                                                     : sizeof(query) - 1;
    if (req->query && qlen > 0) memcpy(query, req->query, qlen);
    query[req->query && qlen > 0 ? qlen : 0] = '\0';
    if (!s_pbf_ctx) {
        send_error(res, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(res, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(res, 400, "Tile coordinates out of range");
        return;
    }

    /* Parse query parameters for ASCII options */
    CTAsciiOptions ascii_opts;
    ct_ascii_default_options(&ascii_opts);

    ascii_opts.width = get_query_int(query, "width", 80, 1, 256);
    ascii_opts.height = get_query_int(query, "height", 0, 0, 256);  /* 0 = auto */
    ascii_opts.invert = get_query_int(query, "invert", 0, 0, 1);
    ascii_opts.color = get_query_int(query, "color", 0, 0, 1);

    /* Parse charset: simple, extended, blocks, braille */
    char charset_buf[16];
    if (sh_query_get_str(query, "charset",
                         charset_buf, sizeof(charset_buf)) > 0) {
        if (strcmp(charset_buf, "simple") == 0) {
            ascii_opts.charset = CT_ASCII_SIMPLE;
        } else if (strcmp(charset_buf, "extended") == 0) {
            ascii_opts.charset = CT_ASCII_EXTENDED;
        } else if (strcmp(charset_buf, "blocks") == 0) {
            ascii_opts.charset = CT_ASCII_BLOCKS;
        } else if (strcmp(charset_buf, "braille") == 0) {
            ascii_opts.charset = CT_ASCII_BRAILLE;
        }
    }

    /* Clamp dimensions to reasonable range */
    if (ascii_opts.width < 20) ascii_opts.width = 20;
    if (ascii_opts.width > 400) ascii_opts.width = 400;
    if (ascii_opts.height > 200) ascii_opts.height = 200;

    RenderCtx *ctx = render_ctx_new(RENDER_TYPE_ASCII, z, x, y, "endpoint:ascii", timer);
    if (!ctx) {
        send_error_cors(res, req, 500, "Memory allocation failed");
        return;
    }
    ctx->ascii_opts = ascii_opts;
    submit_render_work(req, res, ud, ctx);
}

/* GET /tiles/{z}/{x}/{y}.png */
static void handle_png_tile(KlHttpRequest *req, KlHttpResponse *res, void *ud,
                            int z, int x, int y, ShMetricsTimer timer) {
    if (!s_pbf_ctx) {
        send_error(res, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(res, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(res, 400, "Tile coordinates out of range");
        return;
    }

    /* Check cache first (thread-safe) - before queueing */
    if (s_png_cache) {
        const uint8_t *cached_data;
        size_t cached_size;
        pthread_mutex_lock(&s_cache_mutex);
        int hit = ct_cache_get(s_png_cache, z, x, y, &cached_data, &cached_size);
        if (hit) {
            uint8_t *copy = malloc(cached_size);
            if (copy) {
                memcpy(copy, cached_data, cached_size);
                pthread_mutex_unlock(&s_cache_mutex);
                send_tile_cors(res, req, "image/png", copy, cached_size);
                free(copy);
                return;
            }
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    RenderCtx *ctx = render_ctx_new(RENDER_TYPE_PNG, z, x, y, "endpoint:png", timer);
    if (!ctx) {
        send_error_cors(res, req, 500, "Memory allocation failed");
        return;
    }
    submit_render_work(req, res, ud, ctx);
}

/* Parse tile coordinates from URI like /tiles/14/9058/5729.png */
static int parse_tile_uri(const char *uri, size_t uri_len, int *z, int *x, int *y,
                          char *ext) {
    /* Skip /tiles/ prefix */
    if (uri_len < 8) return -1;
    const char *p = uri + 7;  /* Skip "/tiles/" */
    const char *end = uri + uri_len;

    /* Parse z */
    char *next;
    *z = (int)strtol(p, &next, 10);
    if (next == p || *next != '/') return -1;
    p = next + 1;

    /* Parse x */
    *x = (int)strtol(p, &next, 10);
    if (next == p || *next != '/') return -1;
    p = next + 1;

    /* Parse y and extension */
    *y = (int)strtol(p, &next, 10);
    if (next == p) return -1;

    /* Get extension */
    if (*next == '.') {
        next++;
        int i = 0;
        while (next < end && i < 7 && isalnum(*next)) {
            ext[i++] = *next++;
        }
        ext[i] = '\0';
    } else {
        ext[0] = '\0';
    }

    return 0;
}

/* ============================================================================
 * Main Event Handler
 * ============================================================================ */

/* Handle /metrics endpoint for Prometheus - uses shared helper */
static void handle_metrics(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)req; (void)ud;
    sh_kl_handle_metrics(res);
}

/* ============================================================================
 * Static Files
 * ============================================================================ */

/* Minimal content-type table for the assets the map client serves. */
static const char *static_content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".js") == 0)   return "text/javascript; charset=utf-8";
    if (strcmp(dot, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png") == 0)  return "image/png";
    if (strcmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcmp(dot, ".wasm") == 0) return "application/wasm";
    if (strcmp(dot, ".ico") == 0)  return "image/x-icon";
    return "application/octet-stream";
}

/*
 * Serve a file from static_dir. Returns 1 if a response was written.
 *
 * Rejects any path containing "..", so a request cannot escape the root.
 * mg_http_serve_dir() did this internally; with Keel it is our job.
 */
static int serve_static_file(const KlHttpRequest *req, KlHttpResponse *res) {
    char path[SH_PATH_MAX];
    char rel[512];

    size_t len = req->path_len < sizeof(rel) - 1 ? req->path_len : sizeof(rel) - 1;
    memcpy(rel, req->path, len);
    rel[len] = '\0';

    if (strstr(rel, "..")) return 0;              /* no traversal */
    if (strcmp(rel, "/") == 0) snprintf(rel, sizeof(rel), "/index.html");

    int n = snprintf(path, sizeof(path), "%s%s", s_config.server.static_dir, rel);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsize = ftell(f);
    if (fsize < 0 || fsize > 32 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);
    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(f); return 0; }
    size_t size = fread(data, 1, (size_t)fsize, f);
    fclose(f);

    kl_http_response_status(res, 200);
    kl_http_response_header(res, "Content-Type", static_content_type(path));
    sh_kl_apply_cors(res, &s_cors, get_origin_from_request(req));
    kl_http_response_body_copy(res, data, size);
    free(data);
    return 1;
}

/* ============================================================================
 * Middleware and Routing
 * ============================================================================ */

/* CORS preflight, before rate limiting (as in the mongoose server). */
static int mw_preflight(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_kl_trace_header_getter, req);
    sh_kl_reply_preflight(res, &s_cors, get_origin_from_request(req));
    sh_trace_clear();
    return 1;  /* short-circuit */
}

/* Rate limit every request before routing. */
static int mw_rate_limit(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    (void)ud;
    sh_trace_from_headers(sh_kl_trace_header_getter, req);
    if (!sh_kl_check_rate_limit(req, res, s_rate_limiter, &s_cors,
                                get_origin_from_request(req))) {
        SH_LOG_WARN("Rate limit exceeded", "status", "429");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:429", "endpoint:ratelimit", NULL);
        sh_trace_clear();
        return 1;  /* short-circuit */
    }
    return 0;
}

/* Tile routes: /tiles/{z}/{x}/{y}.{ext} */
static void handle_tile(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    ShMetricsTimer timer = sh_metrics_timer_start();
    int z, x, y;
    char ext[8];

    if (parse_tile_uri(req->path, req->path_len, &z, &x, &y, ext) != 0) {
        send_error_cors(res, req, 400, "Invalid tile URL format");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:400", "endpoint:tiles", NULL);
        sh_trace_clear();
        return;
    }

    if (strcmp(ext, "mvt") == 0 || strcmp(ext, "pbf") == 0) {
        handle_mvt_tile(req, res, ud, z, x, y, timer);
    } else if (strcmp(ext, "png") == 0) {
        handle_png_tile(req, res, ud, z, x, y, timer);
    } else if (strcmp(ext, "txt") == 0 || strcmp(ext, "ascii") == 0) {
        handle_ascii_tile(req, res, ud, z, x, y, timer);
    } else {
        send_error_cors(res, req, 400,
                        "Unknown tile format. Use .mvt, .png, .txt, or .ascii");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:400", "endpoint:tiles", NULL);
    }
    sh_trace_clear();
}

/*
 * Anything the route table would not match: static files, else 404.
 *
 * Keel route patterns have no wildcard ('*' is only special in middleware
 * patterns), so the static-file fallback is handled here rather than as a
 * route. Keel's built-in 404 is also text/plain with no CORS headers, which
 * the map client would see as an opaque failure.
 */
static int mw_fallback(KlHttpRequest *req, KlHttpResponse *res, void *ud) {
    AppCtx *app = (AppCtx *)ud;
    KlHttpRoute *matched = NULL;
    KlHttpParam params[KL_HTTP_ROUTER_MAX_PARAMS];
    int num_params = 0;

    int rc = kl_http_router_match(&app->server->router,
                                  req->method, req->method_len,
                                  req->path, req->path_len,
                                  &matched, params, &num_params);
    if (rc == 200) return 0;

    /*
     * A /tiles/ path that did not match the route is malformed (too few or
     * too many segments). The mongoose server answered 400 there; keep that
     * rather than letting it fall through to the static handler.
     */
    if (req->path_len > 7 && memcmp(req->path, "/tiles/", 7) == 0) {
        send_error_cors(res, req, 400, "Invalid tile URL format");
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:400", "endpoint:tiles", NULL);
        sh_trace_clear();
        return 1;
    }

    /* Static files */
    if (s_config.server.static_dir[0] && serve_static_file(req, res)) {
        sh_metrics_counter_inc("http_requests_total", 1,
                               "status:200", "endpoint:static", NULL);
        sh_trace_clear();
        return 1;
    }

    send_error_cors(res, req, 404, "Not found");
    sh_metrics_counter_inc("http_requests_total", 1,
                           "status:404", "endpoint:unknown", NULL);
    sh_trace_clear();
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Carta Tile Server\n\n");
    printf("Usage: %s [options] <pbf-file>\n\n", prog);

    /* Common options from sh_args */
    sh_args_usage(prog, "<pbf-file>");

    printf("Carta-specific options:\n");
    printf("  -c, --config FILE    Configuration file (YAML format)\n");
    printf("  --min-zoom N         Minimum zoom level (default: 0)\n");
    printf("  --max-zoom N         Maximum zoom level (default: 18)\n");
    printf("  --tile-size N        PNG tile size (default: 512)\n");
    printf("  --lod PRESET         LOD filtering: none, default, detailed, minimal\n");
    printf("  --no-lod             Disable LOD filtering (same as --lod none)\n");
    printf("  --render-preset P    Render quality: default, fast, quality\n");
    printf("  -S, --save-index FILE  Save binary index for fast loading\n");
    printf("  --render-workers N   Render worker threads (default: auto)\n");
    printf("\n");
    printf("Carta-specific environment variables:\n");
    printf("  TILE_PBF_PATH, CARTA_DATA_FILE  Path to OSM PBF file\n");
    printf("  CARTA_MIN_ZOOM, TILE_MIN_ZOOM   Minimum zoom\n");
    printf("  CARTA_MAX_ZOOM, TILE_MAX_ZOOM   Maximum zoom\n");
    printf("  CARTA_TILE_SIZE, TILE_SIZE      PNG tile size\n");
    printf("  CARTA_LOD, TILE_LOD             LOD preset (default, detailed, minimal, none)\n");
    printf("  CARTA_RENDER_PRESET             Render preset (default, fast, quality)\n");
    printf("  CARTA_RENDER_WORKERS            Render worker count (0 = auto)\n");
    printf("\n");
    printf("CORS configuration:\n");
    printf("  CARTA_CORS_ORIGINS      Comma-separated allowed origins (empty = allow all)\n");
    printf("  CARTA_CORS_METHODS      Allowed HTTP methods (default: GET, POST, OPTIONS)\n");
    printf("  CARTA_CORS_HEADERS      Allowed request headers\n");
    printf("  CARTA_CORS_CREDENTIALS  Allow credentials (default: 0)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -p 8081 hungary-latest.osm.pbf\n", prog);
    printf("  %s --threads 8 hungary-latest.osm.pbf\n", prog);
    printf("  %s --no-lod hungary-latest.osm.pbf\n", prog);
    printf("  CARTA_CORS_ORIGINS=https://app.example.com %s map.pbf\n", prog);
}

int main(int argc, char *argv[]) {
    /* Initialize logging first (reads SH_LOG_LEVEL, SH_LOG_FORMAT from env) */
    ShLogConfig log_cfg = SH_LOG_CONFIG_DEFAULT;
    log_cfg.service = "carta";
    log_cfg.version = ct_version();
    sh_log_init(&log_cfg);

    /* Initialize defaults */
    init_carta_defaults(&s_config);
    sh_cors_init(&s_cors);

    /* Load config from environment (uses sh_args for common, carta-specific for the rest) */
    load_carta_env(&s_config);

    /* Parse command line arguments using sh_args for common options */
    int arg_index = sh_args_parse(&s_config.server, argc, argv);
    if (arg_index == -2) {
        /* --help was passed to sh_args */
        print_usage(argv[0]);
        return 0;
    }

    /* Parse Carta-specific arguments */
    for (int i = (arg_index > 0 ? arg_index : 1); i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (++i < argc) {
                if (load_config_file(argv[i], &s_config) != 0) {
                    fprintf(stderr, "Warning: Could not load config file: %s\n", argv[i]);
                }
            }
        } else if (strcmp(argv[i], "--min-zoom") == 0) {
            if (++i < argc) s_config.min_zoom = sh_parse_int(argv[i], s_config.min_zoom, 0, 22);
        } else if (strcmp(argv[i], "--max-zoom") == 0) {
            if (++i < argc) s_config.max_zoom = sh_parse_int(argv[i], s_config.max_zoom, 0, 22);
        } else if (strcmp(argv[i], "--tile-size") == 0) {
            if (++i < argc) s_config.tile_size = sh_parse_int(argv[i], s_config.tile_size, 64, 4096);
        } else if (strcmp(argv[i], "--lod") == 0) {
            if (++i < argc) s_config.lod_preset = parse_lod_preset(argv[i]);
        } else if (strcmp(argv[i], "--no-lod") == 0) {
            s_config.lod_preset = LOD_NONE;
        } else if (strcmp(argv[i], "--render-preset") == 0) {
            if (++i < argc) s_config.render_preset = parse_render_preset(argv[i]);
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save-index") == 0) {
            if (++i < argc) {
                strncpy(s_config.save_index_path, argv[i], sizeof(s_config.save_index_path) - 1);
                s_config.save_index_path[sizeof(s_config.save_index_path) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "--render-workers") == 0) {
            if (++i < argc) s_config.render_workers = sh_parse_int(argv[i], s_config.render_workers, 0, 256);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            /* Positional argument - PBF file */
            strncpy(s_config.pbf_path, argv[i], sizeof(s_config.pbf_path) - 1);
            s_config.pbf_path[sizeof(s_config.pbf_path) - 1] = '\0';
        }
    }

    /* Validate config */
    if (s_config.pbf_path[0] == '\0') {
        fprintf(stderr, "Error: No PBF file specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Load data file - auto-detect binary index vs PBF */
    if (ct_is_binary_index(s_config.pbf_path)) {
        /* Fast path: load pre-built binary index via mmap */
        printf("Loading binary index: %s\n", s_config.pbf_path);
        s_pbf_ctx = ct_index_mmap(s_config.pbf_path);
        if (!s_pbf_ctx) {
            fprintf(stderr, "Error: Failed to load binary index: %s\n", s_config.pbf_path);
            return 1;
        }
        printf("Index loaded via mmap (fast startup)\n");
    } else {
        /* Slow path: parse PBF and build index from scratch */
        printf("Loading PBF: %s\n", s_config.pbf_path);

        /* Configure loading with progress callback */
        CTPBFConfig pbf_config;
        ct_pbf_config_init(&pbf_config);
        pbf_config.progress_callback = pbf_progress_callback;
        pbf_config.progress_interval = 50;  /* Report every 50 blobs */

        s_pbf_ctx = ct_load_pbf_with_config(s_config.pbf_path, &pbf_config);
        if (!s_pbf_ctx) {
            fprintf(stderr, "Error: Failed to load PBF file: %s\n", s_config.pbf_path);
            return 1;
        }
        printf("\n");  /* Newline after progress */
    }

    /* Print stats */
    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(s_pbf_ctx, &nodes, &ways, &features, &bbox);
    printf("Loaded: %zu nodes, %zu ways, %zu features indexed\n", nodes, ways, features);
    printf("Multipolygons: %zu\n", s_pbf_ctx->num_multipolygons);
    printf("Bounds: [%.4f, %.4f] to [%.4f, %.4f]\n",
           bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat);

    /* Save index if requested */
    if (s_config.save_index_path[0] != '\0') {
        printf("Saving index to: %s\n", s_config.save_index_path);
        CTStatus save_status = ct_index_save(s_pbf_ctx, s_config.save_index_path);
        if (save_status == CT_OK) {
            printf("Index saved successfully.\n");
        } else {
            fprintf(stderr, "Warning: Failed to save index: %s\n", ct_status_string(save_status));
        }

        /* Exit if --build-only was specified */
        if (s_config.server.build_only) {
            printf("Build complete (--build-only specified)\n");
            ct_free_pbf_context(s_pbf_ctx);
            sh_log_shutdown();
            return 0;
        }
    }

    /* Initialize LOD config based on preset */
    ct_lod_init(&s_lod_config);
    switch (s_config.lod_preset) {
        case LOD_DEFAULT:
            ct_lod_default(&s_lod_config);
            printf("LOD: default (zoom-dependent feature filtering enabled)\n");
            break;
        case LOD_DETAILED:
            ct_lod_detailed(&s_lod_config);
            printf("LOD: detailed (more features at lower zoom)\n");
            break;
        case LOD_MINIMAL:
            ct_lod_minimal(&s_lod_config);
            printf("LOD: minimal (fewer features, overview mode)\n");
            break;
        case LOD_NONE:
        default:
            printf("LOD: disabled (all features at all zoom levels)\n");
            break;
    }

    /* Initialize render options based on preset */
    switch (s_config.render_preset) {
        case RENDER_PRESET_FAST:
            ct_render_options_fast(&s_render_opts);
            printf("Render: fast (no boundaries, casing, or outlines; labels enabled)\n");
            break;
        case RENDER_PRESET_QUALITY:
            ct_render_options_quality(&s_render_opts);
            printf("Render: quality (all effects enabled)\n");
            break;
        case RENDER_PRESET_DEFAULT:
        default:
            ct_render_options_default(&s_render_opts);
            printf("Render: default (balanced rendering)\n");
            break;
    }

    /* Create transport-agnostic API context */
    {
        CTAPIConfig api_config;
        ct_api_config_init(&api_config);
        api_config.min_zoom = s_config.min_zoom;
        api_config.max_zoom = s_config.max_zoom;
        api_config.tile_size = s_config.tile_size;
        api_config.enable_lod = (s_config.lod_preset != LOD_NONE);
        api_config.name = s_config.name;
        api_config.metatile_cache_size = s_config.metatile_cache_size;

        s_api_ctx = ct_api_create_from_pbf(s_pbf_ctx, &api_config);
        if (!s_api_ctx) {
            fprintf(stderr, "Error: Failed to create API context\n");
            ct_free_pbf_context(s_pbf_ctx);
            return 1;
        }

        /* Apply LOD preset (default is already set, apply others) */
        if (s_config.lod_preset == LOD_NONE) {
            ct_api_disable_lod(s_api_ctx);
        } else if (s_config.lod_preset != LOD_DEFAULT) {
            ct_api_set_lod(s_api_ctx, &s_lod_config);
        }

        /* Apply render options */
        ct_api_set_render_opts(s_api_ctx, &s_render_opts);

        if (ct_api_get_metatile_cache(s_api_ctx)) {
            printf("Metatile labels: %zu-entry cache\n",
                   s_config.metatile_cache_size);
        } else {
            printf("Metatile labels: disabled\n");
        }
    }

    /* Initialize tile caches (256MB each by default) */
    s_png_cache = ct_cache_create(256);
    s_mvt_cache = ct_cache_create(256);
    if (s_png_cache && s_mvt_cache) {
        printf("Cache: 256MB PNG + 256MB MVT (512MB total)\n");
    } else {
        printf("Cache: disabled (allocation failed)\n");
    }

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

    /* Render pool is created after the HTTP server (it needs the event ctx). */

    /* Initialize adaptive capacity tracker */
    if (s_config.server.adaptive_enabled) {
        ShAdaptiveConfig adaptive_cfg;
        sh_adaptive_config_init(&adaptive_cfg);
        int num_workers = s_config.render_workers > 0 ? s_config.render_workers : 4;
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
    metrics_cfg.service = "carta";
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
                "pbf", s_config.pbf_path,
                "port", s_config.server.host);


    /* Build listen address (stored globally for worker threads) */
    snprintf(s_listen_url, sizeof(s_listen_url), "http://%s:%d",
             s_config.server.host, s_config.server.port);

    printf("\nCarta Tile Server v%s\n", ct_version());
    printf("Listening on http://%s:%d\n", s_config.server.host, s_config.server.port);

    /* Determine number of worker threads */
    int num_threads = s_config.server.worker_threads;
    if (num_threads <= 0) {
#ifdef _SC_NPROCESSORS_ONLN
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (n > 0) ? (int)n : 4;
#else
        num_threads = 4;
#endif
    }
    if (num_threads > 64) num_threads = 64;

    printf("Workers: %d HTTP handler thread%s\n", num_threads,
           num_threads == 1 ? "" : "s");

    printf("\nEndpoints:\n");
    printf("  GET  /                       - Static files / tile viewer\n");
    printf("  GET  /tiles.json             - TileJSON metadata\n");
    printf("  GET  /tiles/{z}/{x}/{y}.png  - Raster tile\n");
    printf("  GET  /tiles/{z}/{x}/{y}.mvt  - Vector tile\n");
    printf("  GET  /tiles/{z}/{x}/{y}.txt  - ASCII art tile\n");
    printf("  GET  /api/v1/health          - Health check\n");
    printf("  GET  /api/v1/stats           - PBF statistics\n");
    printf("  GET  /metrics                - Prometheus metrics\n");
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Initialize HTTP server */
    KlHttpServer server;
    KlHttpServerConfig http_cfg = {
        .port = s_config.server.port,
        .bind_addr = s_config.server.host,
        .install_signal_handlers = 1,
        .drain_timeout_ms = 5000,
    };

    if (kl_http_server_init(&server, &http_cfg) < 0) {
        fprintf(stderr, "Error: Cannot listen on %s:%d\n",
                s_config.server.host, s_config.server.port);
        sh_ratelimit_free(s_rate_limiter);
        sh_adaptive_free(s_adaptive_tracker);
        ct_cache_free(s_png_cache);
        ct_cache_free(s_mvt_cache);
        ct_lod_free(&s_lod_config);
        ct_api_free(s_api_ctx);
        ct_free_pbf_context(s_pbf_ctx);
        return 1;
    }

    /*
     * Render pool. --threads used to size the number of mongoose event-loop
     * threads; with a non-blocking loop it sizes the render pool instead.
     */
    if (s_config.server.work_queue_enabled) {
        s_num_workers = s_config.render_workers > 0 ? s_config.render_workers
                                                    : num_threads;
        KlThreadPoolConfig pool_cfg = {
            .num_workers = s_num_workers,
            .queue_capacity = (int)s_config.server.work_queue_depth,
        };
        s_pool = kl_thread_pool_create(kl_http_server_event_ctx(&server), &pool_cfg);
        if (s_pool) {
            printf("Render pool: depth %zu, timeout %.1fs, %d render workers\n",
                   s_config.server.work_queue_depth,
                   s_config.server.work_queue_timeout, s_num_workers);
        } else {
            fprintf(stderr, "Warning: Failed to create render thread pool\n");
        }
    }

    AppCtx app = { .server = &server, .pool = s_pool };

    /* Routes. Static files stay in mw_fallback: Keel route patterns have no
       wildcard, so a catch-all is not expressible. */
    kl_http_server_route(&server, "GET", "/api/v1/health", handle_health,   NULL, NULL);
    kl_http_server_route(&server, "GET", "/api/v1/stats",  handle_stats,    NULL, NULL);
    kl_http_server_route(&server, "GET", "/metrics",       handle_metrics,  NULL, NULL);
    kl_http_server_route(&server, "GET", "/tiles.json",    handle_tilejson, NULL, NULL);
    /*
     * Tiles must be a real route, not middleware: kl_async_suspend() is only
     * honoured after a route handler (conn_process checks for SUSPENDED). A
     * middleware short-circuit goes straight to SENDING, so the connection is
     * sent and recycled while the op is still suspended, and done_fn then
     * writes into a freed response. Route params give us the prefix match.
     */
    kl_http_server_route(&server, "GET", "/tiles/:z/:x/:y", handle_tile,   &app, NULL);

    /* Middleware runs in registration order, before routing. */
    kl_http_server_use(&server, "OPTIONS", "/*", mw_preflight, NULL);
    kl_http_server_use(&server, "*", "/*", mw_rate_limit, NULL);
    kl_http_server_use(&server, "*", "/*", mw_fallback, &app);

    /* Event loop: blocks until SIGINT/SIGTERM. */
    kl_http_server_run(&server);

    printf("\nShutting down...\n");

    /* Pool first: drains in-flight renders, fires cancel_fn for queued items. */
    if (s_pool) kl_thread_pool_free(s_pool);
    kl_http_server_free(&server);

    printf("Render queue: %lu pushed, %lu popped, %lu dropped, %lu expired\n",
           (unsigned long)s_qstats.pushed, (unsigned long)s_qstats.popped,
           (unsigned long)s_qstats.dropped, (unsigned long)s_qstats.expired);


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

    /* Print cache stats */
    if (s_png_cache || s_mvt_cache) {
        size_t entries, bytes;
        uint64_t hits, misses;

        if (s_png_cache) {
            ct_cache_stats(s_png_cache, &entries, &bytes, &hits, &misses);
            printf("PNG cache: %zu entries, %.1f MB, %lu hits, %lu misses (%.1f%% hit rate)\n",
                   entries, (double)bytes / (1024*1024),
                   (unsigned long)hits, (unsigned long)misses,
                   (hits + misses) > 0 ? (100.0 * hits / (hits + misses)) : 0.0);
        }
        if (s_mvt_cache) {
            ct_cache_stats(s_mvt_cache, &entries, &bytes, &hits, &misses);
            printf("MVT cache: %zu entries, %.1f MB, %lu hits, %lu misses (%.1f%% hit rate)\n",
                   entries, (double)bytes / (1024*1024),
                   (unsigned long)hits, (unsigned long)misses,
                   (hits + misses) > 0 ? (100.0 * hits / (hits + misses)) : 0.0);
        }
    }

    sh_ratelimit_free(s_rate_limiter);
    sh_adaptive_free(s_adaptive_tracker);
    ct_cache_free(s_png_cache);
    ct_cache_free(s_mvt_cache);
    ct_lod_free(&s_lod_config);
    ct_api_free(s_api_ctx);
    ct_free_pbf_context(s_pbf_ctx);
    pthread_mutex_destroy(&s_cache_mutex);

    SH_LOG_INFO("Server shutdown complete");
    sh_metrics_shutdown();
    sh_log_shutdown();

    return 0;
}
