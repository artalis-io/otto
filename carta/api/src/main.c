/*
 * Carta Tile Server
 *
 * A lightweight tile server that serves vector (MVT), raster (PNG), and
 * ASCII art tiles from OSM PBF files using carta and mongoose.
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
#include <signal.h>
#include <ctype.h>
#include <unistd.h>   /* For sleep, sysconf */
#include <pthread.h>
#include "mongoose.h"
#include "carta.h"
#include "ct_cache.h"

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

typedef struct {
    char pbf_path[512];
    char static_dir[512];
    char listen_addr[64];
    char save_index_path[512];  /* Path to save binary index */
    int port;
    int min_zoom;
    int max_zoom;
    int tile_size;
    char name[128];
    LODPreset lod_preset;  /* LOD filtering preset */
    int num_threads;       /* Worker threads (0 = auto-detect) */
} TileServerConfig;

/* Default configuration */
static TileServerConfig s_config = {
    .pbf_path = "",
    .static_dir = "./static",
    .listen_addr = "0.0.0.0",
    .port = 8081,
    .min_zoom = 0,
    .max_zoom = 18,
    .tile_size = 512,
    .name = "Carta Tile Server",
    .lod_preset = LOD_NONE,  /* LOD disabled by default until rules are improved */
    .num_threads = 0         /* 0 = auto-detect CPU count */
};

/* Global state */
static volatile sig_atomic_t s_signo = 0;
static CTPBFContext *s_pbf_ctx = NULL;
static CTLODConfig s_lod_config = {0};
static CTTileCache *s_png_cache = NULL;
static CTTileCache *s_mvt_cache = NULL;

/* Cache mutex for thread-safe access */
static pthread_mutex_t s_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Worker thread state */
typedef struct {
    int id;
    pthread_t thread;
    struct mg_mgr mgr;
} WorkerThread;

static WorkerThread *s_workers = NULL;
static int s_num_workers = 0;
static char s_listen_url[128] = "";

/* Forward declaration */
static void ev_handler(struct mg_connection *c, int ev, void *ev_data);

/* Worker thread function - runs its own mongoose event loop */
static void *worker_thread_fn(void *arg) {
    WorkerThread *w = (WorkerThread *)arg;

    /* Initialize mongoose manager for this thread */
    mg_mgr_init(&w->mgr);

    /* Listen with SO_REUSEPORT for load balancing across threads */
    struct mg_connection *c = mg_http_listen(&w->mgr, s_listen_url, ev_handler, NULL);
    if (c == NULL) {
        fprintf(stderr, "Worker %d: Cannot listen on %s\n", w->id, s_listen_url);
        return NULL;
    }

    /* Event loop - render contexts are created lazily via thread-local storage */
    while (s_signo == 0) {
        mg_mgr_poll(&w->mgr, 100);
    }

    mg_mgr_free(&w->mgr);

    /* Thread-local render context is freed by pthread_key destructor */
    return NULL;
}

static void signal_handler(int signo) {
    s_signo = signo;
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
            strncpy(cfg->static_dir, value, sizeof(cfg->static_dir) - 1);
            cfg->static_dir[sizeof(cfg->static_dir) - 1] = '\0';
        } else if (strcmp(key, "listen") == 0 || strcmp(key, "host") == 0) {
            strncpy(cfg->listen_addr, value, sizeof(cfg->listen_addr) - 1);
            cfg->listen_addr[sizeof(cfg->listen_addr) - 1] = '\0';
        } else if (strcmp(key, "port") == 0) {
            cfg->port = atoi(value);
        } else if (strcmp(key, "min_zoom") == 0) {
            cfg->min_zoom = atoi(value);
        } else if (strcmp(key, "max_zoom") == 0) {
            cfg->max_zoom = atoi(value);
        } else if (strcmp(key, "tile_size") == 0) {
            cfg->tile_size = atoi(value);
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

/* Load configuration from environment variables */
static void load_config_env(TileServerConfig *cfg) {
    const char *val;

    if ((val = getenv("TILE_PBF_PATH")) || (val = getenv("PBF_PATH"))) {
        strncpy(cfg->pbf_path, val, sizeof(cfg->pbf_path) - 1);
        cfg->pbf_path[sizeof(cfg->pbf_path) - 1] = '\0';
    }
    if ((val = getenv("TILE_STATIC_DIR")) || (val = getenv("STATIC_DIR"))) {
        strncpy(cfg->static_dir, val, sizeof(cfg->static_dir) - 1);
        cfg->static_dir[sizeof(cfg->static_dir) - 1] = '\0';
    }
    if ((val = getenv("TILE_PORT")) || (val = getenv("PORT"))) {
        cfg->port = atoi(val);
    }
    if ((val = getenv("TILE_HOST")) || (val = getenv("HOST"))) {
        strncpy(cfg->listen_addr, val, sizeof(cfg->listen_addr) - 1);
        cfg->listen_addr[sizeof(cfg->listen_addr) - 1] = '\0';
    }
    if ((val = getenv("TILE_MIN_ZOOM"))) {
        cfg->min_zoom = atoi(val);
    }
    if ((val = getenv("TILE_MAX_ZOOM"))) {
        cfg->max_zoom = atoi(val);
    }
    if ((val = getenv("TILE_SIZE"))) {
        cfg->tile_size = atoi(val);
    }
    if ((val = getenv("TILE_NAME"))) {
        strncpy(cfg->name, val, sizeof(cfg->name) - 1);
        cfg->name[sizeof(cfg->name) - 1] = '\0';
    }
    if ((val = getenv("TILE_LOD"))) {
        cfg->lod_preset = parse_lod_preset(val);
    }
    if ((val = getenv("CARTA_THREADS"))) {
        cfg->num_threads = atoi(val);
    }
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

static void send_json(struct mg_connection *c, int status, const char *json) {
    mg_http_reply(c, status,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "%s", json);
}

static void send_error(struct mg_connection *c, int status, const char *message) {
    mg_http_reply(c, status,
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n",
        "{\"error\": \"%s\"}\n", message);
}

static void send_tile(struct mg_connection *c, const char *content_type,
                      const uint8_t *data, size_t size) {
    /* Send HTTP headers manually for binary data */
    /* Note: mongoose printf doesn't support %zu, use %lu with cast */
    /* Use Connection: close to prevent proxy issues with keep-alive */
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: public, max-age=86400\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, (unsigned long)size);
    mg_send(c, data, size);
}

/* ============================================================================
 * API Handlers
 * ============================================================================ */

/* GET /api/v1/health */
static void handle_health(struct mg_connection *c) {
    char response[512];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"carta-tile-server\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        ct_version());
    send_json(c, 200, response);
}

/* GET /api/v1/stats */
static void handle_stats(struct mg_connection *c) {
    if (!s_pbf_ctx) {
        send_error(c, 503, "PBF not loaded");
        return;
    }

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(s_pbf_ctx, &nodes, &ways, &features, &bbox);

    char response[1024];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"pbf_path\": \"%s\",\n"
        "  \"total_nodes\": %zu,\n"
        "  \"total_ways\": %zu,\n"
        "  \"features_indexed\": %zu,\n"
        "  \"bbox\": {\n"
        "    \"min_lat\": %.6f,\n"
        "    \"min_lon\": %.6f,\n"
        "    \"max_lat\": %.6f,\n"
        "    \"max_lon\": %.6f\n"
        "  }\n"
        "}\n",
        s_config.pbf_path, nodes, ways, features,
        bbox.min_lat, bbox.min_lon, bbox.max_lat, bbox.max_lon);
    send_json(c, 200, response);
}

/* GET /tiles.json - TileJSON metadata */
static void handle_tilejson(struct mg_connection *c, struct mg_http_message *hm) {
    if (!s_pbf_ctx) {
        send_error(c, 503, "PBF not loaded");
        return;
    }

    /* Get host header for building tile URLs */
    struct mg_str host = mg_http_get_header(hm, "Host") ?
                         *mg_http_get_header(hm, "Host") : mg_str("localhost:8081");

    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(s_pbf_ctx, &nodes, &ways, &features, &bbox);

    /* Calculate center */
    double center_lat = (bbox.min_lat + bbox.max_lat) / 2.0;
    double center_lon = (bbox.min_lon + bbox.max_lon) / 2.0;

    char response[2048];
    snprintf(response, sizeof(response),
        "{\n"
        "  \"tilejson\": \"3.0.0\",\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Map tiles generated by Carta\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"attribution\": \"OpenStreetMap contributors\",\n"
        "  \"scheme\": \"xyz\",\n"
        "  \"tiles\": [\n"
        "    \"http://%.*s/tiles/{z}/{x}/{y}.png\"\n"
        "  ],\n"
        "  \"vector_tiles\": [\n"
        "    \"http://%.*s/tiles/{z}/{x}/{y}.mvt\"\n"
        "  ],\n"
        "  \"minzoom\": %d,\n"
        "  \"maxzoom\": %d,\n"
        "  \"bounds\": [%.6f, %.6f, %.6f, %.6f],\n"
        "  \"center\": [%.6f, %.6f, 10]\n"
        "}\n",
        s_config.name,
        (int)host.len, host.buf,
        (int)host.len, host.buf,
        s_config.min_zoom, s_config.max_zoom,
        bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat,
        center_lon, center_lat);
    send_json(c, 200, response);
}

/* GET /tiles/{z}/{x}/{y}.mvt */
static void handle_mvt_tile(struct mg_connection *c, int z, int x, int y) {
    if (!s_pbf_ctx) {
        send_error(c, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(c, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(c, 400, "Tile coordinates out of range");
        return;
    }

    /* Check cache first (thread-safe) */
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
                send_tile(c, "application/vnd.mapbox-vector-tile", copy, cached_size);
                free(copy);
                return;
            }
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    /* Generate MVT tile directly */
    size_t capacity = 512 * 1024;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    CTTileCoord coord = {z, x, y};
    CTMVTOptions opts;
    ct_mvt_default_options(&opts);

    size_t size = ct_generate_mvt(s_pbf_ctx, coord, &opts, buffer, capacity);

    if (size == 0) {
        free(buffer);
        static const uint8_t empty_mvt[] = {0x1a, 0x00};
        send_tile(c, "application/vnd.mapbox-vector-tile", empty_mvt, 0);
        return;
    }

    /* Cache the result (thread-safe) */
    if (s_mvt_cache) {
        pthread_mutex_lock(&s_cache_mutex);
        ct_cache_put(s_mvt_cache, z, x, y, buffer, size);
        pthread_mutex_unlock(&s_cache_mutex);
    }

    send_tile(c, "application/vnd.mapbox-vector-tile", buffer, size);
    free(buffer);
}

/* Parse query string for a parameter, returns default if not found */
static int get_query_int(struct mg_str query, const char *name, int default_val) {
    char buf[32];
    if (mg_http_get_var(&query, name, buf, sizeof(buf)) > 0) {
        return atoi(buf);
    }
    return default_val;
}

/* GET /tiles/{z}/{x}/{y}.txt or .ascii */
static void handle_ascii_tile(struct mg_connection *c, struct mg_http_message *hm,
                              int z, int x, int y) {
    if (!s_pbf_ctx) {
        send_error(c, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(c, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(c, 400, "Tile coordinates out of range");
        return;
    }

    /* Parse query parameters for ASCII options */
    CTAsciiOptions ascii_opts;
    ct_ascii_default_options(&ascii_opts);

    ascii_opts.width = get_query_int(hm->query, "width", 80);
    ascii_opts.height = get_query_int(hm->query, "height", 0);  /* 0 = auto */
    ascii_opts.invert = get_query_int(hm->query, "invert", 0);
    ascii_opts.color = get_query_int(hm->query, "color", 0);

    /* Parse charset: simple, extended, blocks, braille */
    char charset_buf[16];
    if (mg_http_get_var(&hm->query, "charset", charset_buf, sizeof(charset_buf)) > 0) {
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

    /* First render the tile to pixels */
    int tile_size = 512;  /* Use 512 for better detail */

    CTTileCoord coord = {z, x, y};

    /* Create render context */
    CTRenderContext *render_ctx = ct_render_create(tile_size, tile_size);
    if (!render_ctx) {
        send_error(c, 500, "Render context creation failed");
        return;
    }

    ct_render_clear(render_ctx);

    /* Render from PBF context */
    ct_render_from_pbf(render_ctx, s_pbf_ctx, coord);

    /* Get pixel buffer */
    const uint8_t *pixels = ct_render_pixels(render_ctx);

    /* Allocate ASCII buffer */
    size_t ascii_size = ct_ascii_buffer_size(ascii_opts.width,
                                             ascii_opts.height > 0 ? ascii_opts.height : ascii_opts.width / 2,
                                             ascii_opts.charset, ascii_opts.color);
    char *ascii_buf = malloc(ascii_size);
    if (!ascii_buf) {
        ct_render_free(render_ctx);
        send_error(c, 500, "ASCII buffer allocation failed");
        return;
    }

    /* Render to ASCII */
    size_t ascii_len = ct_render_ascii(pixels, tile_size, tile_size,
                                       &ascii_opts, ascii_buf, ascii_size);

    /* Send response */
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %lu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: public, max-age=86400\r\n"
        "Connection: close\r\n"
        "\r\n",
        (unsigned long)ascii_len);
    mg_send(c, ascii_buf, ascii_len);

    /* Cleanup */
    free(ascii_buf);
    ct_render_free(render_ctx);
}

/* Thread-local key for render context */
static pthread_key_t s_render_ctx_key;
static pthread_once_t s_render_ctx_key_once = PTHREAD_ONCE_INIT;

static void render_ctx_destructor(void *ptr) {
    if (ptr) ct_render_free((CTRenderContext *)ptr);
}

static void create_render_ctx_key(void) {
    pthread_key_create(&s_render_ctx_key, render_ctx_destructor);
}

/* Get or create thread-local render context */
static CTRenderContext *get_thread_render_ctx(int tile_size) {
    pthread_once(&s_render_ctx_key_once, create_render_ctx_key);

    CTRenderContext *ctx = pthread_getspecific(s_render_ctx_key);
    if (!ctx) {
        ctx = ct_render_create(tile_size, tile_size);
        if (ctx) {
            pthread_setspecific(s_render_ctx_key, ctx);
        }
    }
    return ctx;
}

/* GET /tiles/{z}/{x}/{y}.png */
static void handle_png_tile(struct mg_connection *c, int z, int x, int y) {
    if (!s_pbf_ctx) {
        send_error(c, 503, "PBF not loaded");
        return;
    }

    if (z < s_config.min_zoom || z > s_config.max_zoom || z > 30) {
        send_error(c, 400, "Zoom out of range");
        return;
    }

    int max_coord = 1 << z;  /* Safe: z <= 30 */
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
        send_error(c, 400, "Tile coordinates out of range");
        return;
    }

    /* Check cache first (thread-safe) */
    if (s_png_cache) {
        const uint8_t *cached_data;
        size_t cached_size;
        pthread_mutex_lock(&s_cache_mutex);
        int hit = ct_cache_get(s_png_cache, z, x, y, &cached_data, &cached_size);
        if (hit) {
            /* Copy data before unlocking - cache data may be evicted */
            uint8_t *copy = malloc(cached_size);
            if (copy) {
                memcpy(copy, cached_data, cached_size);
                pthread_mutex_unlock(&s_cache_mutex);
                send_tile(c, "image/png", copy, cached_size);
                free(copy);
                return;
            }
        }
        pthread_mutex_unlock(&s_cache_mutex);
    }

    CTTileCoord coord = {z, x, y};

    /* Use thread-local render context for efficiency */
    CTRenderContext *render = get_thread_render_ctx(s_config.tile_size);
    if (!render) {
        send_error(c, 500, "Render context creation failed");
        return;
    }

    /* Render tile */
    ct_render_clear(render);
    if (s_config.lod_preset != LOD_NONE) {
        ct_render_from_pbf_lod(render, s_pbf_ctx, coord, &s_lod_config);
    } else {
        ct_render_from_pbf(render, s_pbf_ctx, coord);
    }

    /* Encode to PNG */
    size_t capacity = ct_png_max_size(s_config.tile_size, s_config.tile_size);
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        send_error(c, 500, "Memory allocation failed");
        return;
    }

    CTPNGOptions opts;
    ct_png_default_options(&opts);
    opts.tile_size = s_config.tile_size;

    size_t size = ct_encode_png(ct_render_pixels(render),
                                s_config.tile_size, s_config.tile_size,
                                &opts, buffer, capacity);

    if (size == 0) {
        send_error(c, 500, "Tile generation failed");
        free(buffer);
        return;
    }

    /* Cache the result (thread-safe) */
    if (s_png_cache) {
        pthread_mutex_lock(&s_cache_mutex);
        ct_cache_put(s_png_cache, z, x, y, buffer, size);
        pthread_mutex_unlock(&s_cache_mutex);
    }

    send_tile(c, "image/png", buffer, size);
    free(buffer);
}

/* Parse tile coordinates from URI like /tiles/14/9058/5729.png */
static int parse_tile_uri(struct mg_str uri, int *z, int *x, int *y, char *ext) {
    /* Skip /tiles/ prefix */
    if (uri.len < 8) return -1;
    const char *p = uri.buf + 7;  /* Skip "/tiles/" */
    const char *end = uri.buf + uri.len;

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

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204,
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Access-Control-Max-Age: 86400\r\n",
                "");
            return;
        }

        /* Route requests */
        if (mg_match(hm->uri, mg_str("/api/v1/health"), NULL)) {
            handle_health(c);
        } else if (mg_match(hm->uri, mg_str("/api/v1/stats"), NULL)) {
            handle_stats(c);
        } else if (mg_match(hm->uri, mg_str("/tiles.json"), NULL)) {
            handle_tilejson(c, hm);
        } else if (hm->uri.len > 7 && strncmp(hm->uri.buf, "/tiles/", 7) == 0) {
            /* Parse tile request: /tiles/{z}/{x}/{y}.{ext} */
            int z, x, y;
            char ext[8];
            if (parse_tile_uri(hm->uri, &z, &x, &y, ext) == 0) {
                if (strcmp(ext, "mvt") == 0 || strcmp(ext, "pbf") == 0) {
                    handle_mvt_tile(c, z, x, y);
                } else if (strcmp(ext, "png") == 0) {
                    handle_png_tile(c, z, x, y);
                } else if (strcmp(ext, "txt") == 0 || strcmp(ext, "ascii") == 0) {
                    handle_ascii_tile(c, hm, z, x, y);
                } else {
                    send_error(c, 400, "Unknown tile format. Use .mvt, .png, .txt, or .ascii");
                }
            } else {
                send_error(c, 400, "Invalid tile URL format");
            }
        } else {
            /* Serve static files */
            struct mg_http_serve_opts opts = {
                .root_dir = s_config.static_dir,
                .extra_headers = "Access-Control-Allow-Origin: *\r\n"
            };
            mg_http_serve_dir(c, hm, &opts);
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Carta Tile Server\n\n");
    printf("Usage: %s [options] <pbf-file>\n\n", prog);
    printf("Options:\n");
    printf("  -p, --port PORT      Port to listen on (default: 8081)\n");
    printf("  -h, --host HOST      Host to bind to (default: 0.0.0.0)\n");
    printf("  -s, --static DIR     Static files directory (default: ./static)\n");
    printf("  -c, --config FILE    Configuration file (YAML format)\n");
    printf("  -t, --threads N      Worker threads (default: auto-detect CPU count)\n");
    printf("  --min-zoom N         Minimum zoom level (default: 0)\n");
    printf("  --max-zoom N         Maximum zoom level (default: 18)\n");
    printf("  --tile-size N        PNG tile size (default: 512)\n");
    printf("  --lod PRESET         LOD filtering: none, default, detailed, minimal\n");
    printf("  --no-lod             Disable LOD filtering (same as --lod none)\n");
    printf("  -S, --save-index FILE  Save binary index for fast loading\n");
    printf("  --help               Show this help\n");
    printf("\n");
    printf("Environment variables:\n");
    printf("  TILE_PBF_PATH, PBF_PATH     Path to OSM PBF file\n");
    printf("  TILE_PORT, PORT             Server port\n");
    printf("  TILE_HOST, HOST             Server host\n");
    printf("  TILE_STATIC_DIR             Static files directory\n");
    printf("  TILE_MIN_ZOOM               Minimum zoom\n");
    printf("  TILE_MAX_ZOOM               Maximum zoom\n");
    printf("  TILE_SIZE                   PNG tile size\n");
    printf("  TILE_LOD                    LOD preset (default, detailed, minimal, none)\n");
    printf("  CARTA_THREADS               Worker thread count (0 = auto)\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -p 8081 hungary-latest.osm.pbf\n", prog);
    printf("  %s --threads 8 hungary-latest.osm.pbf\n", prog);
    printf("  %s --no-lod hungary-latest.osm.pbf\n", prog);
}

int main(int argc, char *argv[]) {
    /* Load config from environment first */
    load_config_env(&s_config);

    /* Parse command line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (++i < argc) s_config.port = atoi(argv[i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) {
            if (++i < argc) {
                strncpy(s_config.listen_addr, argv[i], sizeof(s_config.listen_addr) - 1);
                s_config.listen_addr[sizeof(s_config.listen_addr) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--static") == 0) {
            if (++i < argc) {
                strncpy(s_config.static_dir, argv[i], sizeof(s_config.static_dir) - 1);
                s_config.static_dir[sizeof(s_config.static_dir) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) {
            if (++i < argc) {
                if (load_config_file(argv[i], &s_config) != 0) {
                    fprintf(stderr, "Warning: Could not load config file: %s\n", argv[i]);
                }
            }
        } else if (strcmp(argv[i], "--min-zoom") == 0) {
            if (++i < argc) s_config.min_zoom = atoi(argv[i]);
        } else if (strcmp(argv[i], "--max-zoom") == 0) {
            if (++i < argc) s_config.max_zoom = atoi(argv[i]);
        } else if (strcmp(argv[i], "--tile-size") == 0) {
            if (++i < argc) s_config.tile_size = atoi(argv[i]);
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (++i < argc) s_config.num_threads = atoi(argv[i]);
        } else if (strcmp(argv[i], "--lod") == 0) {
            if (++i < argc) s_config.lod_preset = parse_lod_preset(argv[i]);
        } else if (strcmp(argv[i], "--no-lod") == 0) {
            s_config.lod_preset = LOD_NONE;
        } else if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--save-index") == 0) {
            if (++i < argc) {
                strncpy(s_config.save_index_path, argv[i], sizeof(s_config.save_index_path) - 1);
                s_config.save_index_path[sizeof(s_config.save_index_path) - 1] = '\0';
            }
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

    /* Load PBF file */
    printf("Loading PBF: %s\n", s_config.pbf_path);
    s_pbf_ctx = ct_load_pbf(s_config.pbf_path);
    if (!s_pbf_ctx) {
        fprintf(stderr, "Error: Failed to load PBF file: %s\n", s_config.pbf_path);
        return 1;
    }

    /* Print stats */
    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(s_pbf_ctx, &nodes, &ways, &features, &bbox);
    printf("Loaded: %zu nodes, %zu ways, %zu features indexed\n", nodes, ways, features);
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

    /* Initialize tile caches (256MB each by default) */
    s_png_cache = ct_cache_create(256);
    s_mvt_cache = ct_cache_create(256);
    if (s_png_cache && s_mvt_cache) {
        printf("Cache: 256MB PNG + 256MB MVT (512MB total)\n");
    } else {
        printf("Cache: disabled (allocation failed)\n");
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Build listen address (stored globally for worker threads) */
    snprintf(s_listen_url, sizeof(s_listen_url), "http://%s:%d",
             s_config.listen_addr, s_config.port);

    printf("\nCarta Tile Server v%s\n", ct_version());
    printf("Listening on http://%s:%d\n", s_config.listen_addr, s_config.port);

    /* Determine number of worker threads */
    int num_threads = s_config.num_threads;
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
    printf("\nPress Ctrl+C to stop.\n\n");

    /* Allocate worker threads */
    s_num_workers = num_threads;
    s_workers = calloc(num_threads, sizeof(WorkerThread));
    if (!s_workers) {
        fprintf(stderr, "Error: Failed to allocate worker threads\n");
        ct_cache_free(s_png_cache);
        ct_cache_free(s_mvt_cache);
        ct_lod_free(&s_lod_config);
        ct_free_pbf_context(s_pbf_ctx);
        return 1;
    }

    /* Start worker threads */
    int threads_created = 0;
    for (int i = 0; i < num_threads; i++) {
        s_workers[i].id = i;
        s_workers[i].thread = 0;  /* Mark as not created */
        if (pthread_create(&s_workers[i].thread, NULL, worker_thread_fn, &s_workers[i]) != 0) {
            fprintf(stderr, "Error: Failed to create worker thread %d\n", i);
            s_signo = 1;  /* Signal other threads to stop */
            break;
        }
        threads_created++;
    }

    /* Wait for shutdown signal */
    while (s_signo == 0) {
        sleep(1);
    }

    printf("\nShutting down...\n");

    /* Wait for all successfully created worker threads */
    for (int i = 0; i < threads_created; i++) {
        pthread_join(s_workers[i].thread, NULL);
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

    free(s_workers);
    ct_cache_free(s_png_cache);
    ct_cache_free(s_mvt_cache);
    ct_lod_free(&s_lod_config);
    ct_free_pbf_context(s_pbf_ctx);
    pthread_mutex_destroy(&s_cache_mutex);

    return 0;
}
