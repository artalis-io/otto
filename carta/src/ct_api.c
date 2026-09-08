/*
 * Carta API Handler Implementation
 *
 * Transport-agnostic request handling for Carta tile generation.
 * Used by both the Keel HTTP server and WASM exports.
 */

#include "carta.h"
#include "ct_api.h"
#include "ct_ascii.h"
#include "sh_query.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ============================================================================
 * API Context
 * ============================================================================ */

struct CTAPIContext {
    CTPBFContext *pbf;          /* PBF context */
    int owns_pbf;               /* 1 if we should free pbf on destroy */
    CTLODConfig lod_config;     /* LOD filtering config */
    CTRenderOptions render_opts; /* Render quality options */
    CTMetatileLabelCache *mt_cache; /* Metatile label cache (NULL = per-tile labels) */
    int min_zoom;
    int max_zoom;
    int tile_size;
    char name[128];             /* Server name for TileJSON */
};

void ct_api_config_init(CTAPIConfig *config) {
    if (!config) return;
    config->min_zoom = 0;
    config->max_zoom = 18;
    config->tile_size = 512;
    config->enable_lod = 1;
    config->name = "Carta Tile Server";
    config->metatile_cache_size = CT_METATILE_CACHE_DEFAULT;
}

CTAPIContext *ct_api_create(const uint8_t *pbf_data, size_t pbf_len,
                            const CTAPIConfig *config) {
    if (!pbf_data || pbf_len == 0) return NULL;

    /* Load PBF from memory */
    CTPBFContext *pbf = ct_load_pbf_memory(pbf_data, pbf_len);
    if (!pbf) return NULL;

    CTAPIContext *ctx = ct_api_create_from_pbf(pbf, config);
    if (!ctx) {
        ct_free_pbf_context(pbf);
        return NULL;
    }

    ctx->owns_pbf = 1;  /* We own the PBF context */
    return ctx;
}

CTAPIContext *ct_api_create_from_pbf(CTPBFContext *pbf_ctx,
                                     const CTAPIConfig *config) {
    if (!pbf_ctx) return NULL;

    CTAPIContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->pbf = pbf_ctx;
    ctx->owns_pbf = 0;  /* Caller owns the PBF context */

    /* Apply config */
    CTAPIConfig default_config;
    if (!config) {
        ct_api_config_init(&default_config);
        config = &default_config;
    }

    ctx->min_zoom = config->min_zoom;
    ctx->max_zoom = config->max_zoom;
    ctx->tile_size = config->tile_size;

    if (config->name) {
        strncpy(ctx->name, config->name, sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    } else {
        strncpy(ctx->name, "Carta Tile Server", sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    }

    /* Initialize LOD config */
    ct_lod_init(&ctx->lod_config);
    if (config->enable_lod) {
        ct_lod_default(&ctx->lod_config);
    }

    /* Initialize render options */
    ct_render_options_default(&ctx->render_opts);

    /* Initialize metatile label cache */
    if (config->metatile_cache_size > 0) {
        ctx->mt_cache = ct_metatile_cache_create(config->metatile_cache_size);
    }

    return ctx;
}

void ct_api_free(CTAPIContext *ctx) {
    if (!ctx) return;

    ct_metatile_cache_free(ctx->mt_cache);
    ct_lod_free(&ctx->lod_config);

    if (ctx->owns_pbf && ctx->pbf) {
        ct_free_pbf_context(ctx->pbf);
    }

    free(ctx);
}

CTPBFContext *ct_api_get_pbf(CTAPIContext *ctx) {
    return ctx ? ctx->pbf : NULL;
}

void ct_api_set_lod(CTAPIContext *ctx, const CTLODConfig *lod) {
    if (!ctx || !lod) return;
    ct_lod_free(&ctx->lod_config);
    ct_lod_copy(&ctx->lod_config, lod);
}

void ct_api_set_render_opts(CTAPIContext *ctx, const CTRenderOptions *opts) {
    if (!ctx || !opts) return;
    ctx->render_opts = *opts;
}

void ct_api_disable_lod(CTAPIContext *ctx) {
    if (!ctx) return;
    ct_lod_free(&ctx->lod_config);
    ct_lod_init(&ctx->lod_config);  /* Reset to empty = no filtering */
}

CTMetatileLabelCache *ct_api_get_metatile_cache(CTAPIContext *ctx) {
    return ctx ? ctx->mt_cache : NULL;
}

/* ============================================================================
 * Tile Generation
 * ============================================================================ */

uint8_t *ct_api_generate_png(CTAPIContext *ctx,
                             int z, int x, int y,
                             size_t *out_len) {
    if (!ctx || !ctx->pbf || !out_len) return NULL;
    *out_len = 0;

    /* Validate coordinates */
    if (z < ctx->min_zoom || z > ctx->max_zoom || z > 30) return NULL;
    int max_coord = (int)sh_tiles_per_axis(z);
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) return NULL;

    /* Create render context */
    CTRenderContext *render = ct_render_create(ctx->tile_size, ctx->tile_size);
    if (!render) return NULL;

    /* Apply render options */
    ct_render_set_options(render, &ctx->render_opts);

    /* Render tile */
    CTTileCoord coord = {z, x, y};
    ct_render_clear(render);
    ct_render_from_pbf_lod_mt(render, ctx->pbf, coord, &ctx->lod_config,
                               ctx->mt_cache);

    /* Encode to PNG */
    size_t capacity = ct_png_max_size(ctx->tile_size, ctx->tile_size);
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        ct_render_free(render);
        return NULL;
    }

    CTPNGOptions opts;
    ct_png_default_options(&opts);
    opts.tile_size = ctx->tile_size;

    size_t size = ct_encode_png(ct_render_pixels(render),
                                ctx->tile_size, ctx->tile_size,
                                &opts, buffer, capacity);

    ct_render_free(render);

    if (size == 0) {
        free(buffer);
        return NULL;
    }

    *out_len = size;
    return buffer;
}

uint8_t *ct_api_generate_mvt(CTAPIContext *ctx,
                             int z, int x, int y,
                             size_t *out_len) {
    if (!ctx || !ctx->pbf || !out_len) return NULL;
    *out_len = 0;

    /* Validate coordinates */
    if (z < ctx->min_zoom || z > ctx->max_zoom || z > 30) return NULL;
    int max_coord = (int)sh_tiles_per_axis(z);
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) return NULL;

    /* Generate MVT */
    size_t capacity = 2 * 1024 * 1024;  /* 2MB */
    uint8_t *buffer = malloc(capacity);
    if (!buffer) return NULL;

    CTTileCoord coord = {z, x, y};
    CTMVTOptions opts;
    ct_mvt_default_options(&opts);

    size_t size = ct_generate_mvt(ctx->pbf, coord, &opts, &ctx->lod_config,
                                  buffer, capacity);

    if (size == 0) {
        /* Return minimal valid MVT for empty tile */
        buffer[0] = 0x1a;
        buffer[1] = 0x00;
        *out_len = 2;
        return buffer;
    }

    *out_len = size;
    return buffer;
}

char *ct_api_generate_tilejson(CTAPIContext *ctx,
                               const char *host,
                               size_t *out_len) {
    if (!ctx || !ctx->pbf || !out_len) return NULL;
    *out_len = 0;

    if (!host) host = "localhost";

    /* Get PBF stats */
    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(ctx->pbf, &nodes, &ways, &features, &bbox);

    /* Calculate center */
    double center_lat = (bbox.min_lat + bbox.max_lat) / 2.0;
    double center_lon = (bbox.min_lon + bbox.max_lon) / 2.0;

    /* Build JSON */
    char *buffer = malloc(2048);
    if (!buffer) return NULL;

    int len = snprintf(buffer, 2048,
        "{\n"
        "  \"tilejson\": \"3.0.0\",\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Map tiles generated by Carta\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"attribution\": \"OpenStreetMap contributors\",\n"
        "  \"scheme\": \"xyz\",\n"
        "  \"tiles\": [\n"
        "    \"http://%s/tiles/{z}/{x}/{y}.png\"\n"
        "  ],\n"
        "  \"vector_tiles\": [\n"
        "    \"http://%s/tiles/{z}/{x}/{y}.mvt\"\n"
        "  ],\n"
        "  \"minzoom\": %d,\n"
        "  \"maxzoom\": %d,\n"
        "  \"bounds\": [%.6f, %.6f, %.6f, %.6f],\n"
        "  \"center\": [%.6f, %.6f, 10]\n"
        "}\n",
        ctx->name, host, host,
        ctx->min_zoom, ctx->max_zoom,
        bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat,
        center_lon, center_lat);

    if (len < 0 || len >= 2048) {
        free(buffer);
        return NULL;
    }
    *out_len = (size_t)len;
    return buffer;
}

char *ct_api_generate_health(CTAPIContext *ctx, size_t *out_len) {
    if (!out_len) return NULL;
    *out_len = 0;

    char *buffer = malloc(256);
    if (!buffer) return NULL;

    int len = snprintf(buffer, 256,
        "{\n"
        "  \"status\": \"healthy\",\n"
        "  \"service\": \"carta-tile-server\",\n"
        "  \"version\": \"%s\"\n"
        "}\n",
        ct_version());

    if (len < 0 || len >= 256) {
        free(buffer);
        return NULL;
    }
    *out_len = (size_t)len;
    return buffer;

    (void)ctx;  /* May use ctx for more detailed health in future */
}

char *ct_api_generate_stats(CTAPIContext *ctx, size_t *out_len) {
    if (!ctx || !ctx->pbf || !out_len) return NULL;
    *out_len = 0;

    /* Get PBF stats */
    size_t nodes, ways, features;
    CTBBox bbox;
    ct_pbf_stats(ctx->pbf, &nodes, &ways, &features, &bbox);

    char *buffer = malloc(1024);
    if (!buffer) return NULL;

    int len = snprintf(buffer, 1024,
        "{\n"
        "  \"pbf\": {\n"
        "    \"nodes\": %zu,\n"
        "    \"ways\": %zu,\n"
        "    \"features\": %zu,\n"
        "    \"multipolygons\": %zu,\n"
        "    \"bbox\": [%.6f, %.6f, %.6f, %.6f]\n"
        "  },\n"
        "  \"config\": {\n"
        "    \"min_zoom\": %d,\n"
        "    \"max_zoom\": %d,\n"
        "    \"tile_size\": %d\n"
        "  }\n"
        "}\n",
        nodes, ways, features, ctx->pbf->num_multipolygons,
        bbox.min_lon, bbox.min_lat, bbox.max_lon, bbox.max_lat,
        ctx->min_zoom, ctx->max_zoom, ctx->tile_size);

    if (len < 0 || len >= 1024) {
        free(buffer);
        return NULL;
    }
    *out_len = (size_t)len;
    return buffer;
}

char *ct_api_generate_ascii(CTAPIContext *ctx,
                            int z, int x, int y,
                            const char *query,
                            size_t *out_len) {
    if (!ctx || !ctx->pbf || !out_len) return NULL;
    *out_len = 0;

    /* Validate coordinates */
    if (z < ctx->min_zoom || z > ctx->max_zoom || z > 30) return NULL;
    int max_coord = (int)sh_tiles_per_axis(z);
    if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) return NULL;

    /* Parse ASCII options from query string */
    CTAsciiOptions ascii_opts;
    ct_ascii_default_options(&ascii_opts);

    ascii_opts.width = sh_query_get_int(query, "width", 80);
    ascii_opts.height = sh_query_get_int(query, "height", 0);  /* 0 = auto */
    ascii_opts.invert = sh_query_get_int(query, "invert", 0);
    ascii_opts.color = sh_query_get_int(query, "color", 0);

    /* Parse charset: simple, extended, blocks, braille */
    char charset_buf[16];
    if (sh_query_get_str(query, "charset", charset_buf, sizeof(charset_buf)) > 0) {
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

    /* Create render context */
    CTRenderContext *render = ct_render_create(ctx->tile_size, ctx->tile_size);
    if (!render) return NULL;

    /* Apply render options */
    ct_render_set_options(render, &ctx->render_opts);

    /* Render tile */
    CTTileCoord coord = {z, x, y};
    ct_render_clear(render);
    ct_render_from_pbf_lod_mt(render, ctx->pbf, coord, &ctx->lod_config,
                               ctx->mt_cache);

    /* Get pixels */
    const uint8_t *pixels = ct_render_pixels(render);

    /* Calculate buffer size and allocate */
    int out_height = ascii_opts.height > 0 ? ascii_opts.height : ascii_opts.width / 2;
    size_t buf_size = ct_ascii_buffer_size(ascii_opts.width, out_height,
                                            ascii_opts.charset, ascii_opts.color);
    char *buffer = malloc(buf_size);
    if (!buffer) {
        ct_render_free(render);
        return NULL;
    }

    /* Render to ASCII */
    size_t len = ct_render_ascii(pixels, ctx->tile_size, ctx->tile_size,
                                  &ascii_opts, buffer, buf_size);

    ct_render_free(render);

    if (len == 0) {
        free(buffer);
        return NULL;
    }

    *out_len = len;
    return buffer;
}

/* ============================================================================
 * Path Parsing
 * ============================================================================ */

/*
 * Parse tile coordinates from path like "/tiles/14/8529/5974.png"
 * Returns 0 on success, -1 on failure.
 */
static int parse_tile_path(const char *path, int *z, int *x, int *y, char *ext) {
    if (!path || !z || !x || !y || !ext) return -1;

    /* Skip /tiles/ prefix */
    if (strncmp(path, "/tiles/", 7) != 0) return -1;
    const char *p = path + 7;

    /* Parse z */
    char *next;
    *z = (int)strtol(p, &next, 10);
    if (next == p || *next != '/') return -1;
    p = next + 1;

    /* Parse x */
    *x = (int)strtol(p, &next, 10);
    if (next == p || *next != '/') return -1;
    p = next + 1;

    /* Parse y */
    *y = (int)strtol(p, &next, 10);
    if (next == p) return -1;

    /* Get extension */
    if (*next == '.') {
        next++;
        int i = 0;
        while (*next && i < 7 && isalnum((unsigned char)*next)) {
            ext[i++] = *next++;
        }
        ext[i] = '\0';
    } else {
        ext[0] = '\0';
    }

    return 0;
}

/* ============================================================================
 * Request Handling
 * ============================================================================ */

int ct_api_handle(void *ctx_void,
                  const ShApiRequest *req,
                  ShApiResponse *resp) {
    CTAPIContext *ctx = (CTAPIContext *)ctx_void;

    if (!ctx || !req || !resp || !req->path) return -1;

    /* Initialize response */
    memset(resp, 0, sizeof(*resp));
    resp->status_code = 500;
    resp->content_type = "text/plain";

    /* Route: /api/v1/health */
    if (strcmp(req->path, "/api/v1/health") == 0) {
        resp->body = (uint8_t *)ct_api_generate_health(ctx, &resp->body_len);
        if (resp->body) {
            resp->status_code = 200;
            resp->content_type = "application/json";
        }
        return 0;
    }

    /* Route: /api/v1/stats */
    if (strcmp(req->path, "/api/v1/stats") == 0) {
        resp->body = (uint8_t *)ct_api_generate_stats(ctx, &resp->body_len);
        if (resp->body) {
            resp->status_code = 200;
            resp->content_type = "application/json";
        }
        return 0;
    }

    /* Route: /tiles.json */
    if (strcmp(req->path, "/tiles.json") == 0) {
        const char *host = req->host ? req->host : "localhost";
        resp->body = (uint8_t *)ct_api_generate_tilejson(ctx, host, &resp->body_len);
        if (resp->body) {
            resp->status_code = 200;
            resp->content_type = "application/json";
        }
        return 0;
    }

    /* Route: /tiles/{z}/{x}/{y}.{ext} */
    if (strncmp(req->path, "/tiles/", 7) == 0) {
        int z, x, y;
        char ext[8];

        if (parse_tile_path(req->path, &z, &x, &y, ext) != 0) {
            return sh_api_response_error(resp, 400, "Invalid tile path format");
        }

        /* Validate zoom */
        if (z < ctx->min_zoom || z > ctx->max_zoom) {
            return sh_api_response_error(resp, 400, "Zoom level out of range");
        }

        /* Validate coordinates */
        int max_coord = (int)sh_tiles_per_axis(z);
        if (x < 0 || x >= max_coord || y < 0 || y >= max_coord) {
            return sh_api_response_error(resp, 400, "Tile coordinates out of range");
        }

        /* Generate tile based on extension */
        if (strcmp(ext, "png") == 0) {
            resp->body = ct_api_generate_png(ctx, z, x, y, &resp->body_len);
            if (resp->body) {
                resp->status_code = 200;
                resp->content_type = "image/png";
            } else {
                return sh_api_response_error(resp, 500, "Tile generation failed");
            }
        } else if (strcmp(ext, "mvt") == 0 || strcmp(ext, "pbf") == 0) {
            resp->body = ct_api_generate_mvt(ctx, z, x, y, &resp->body_len);
            if (resp->body) {
                resp->status_code = 200;
                resp->content_type = "application/vnd.mapbox-vector-tile";
            } else {
                return sh_api_response_error(resp, 500, "Tile generation failed");
            }
        } else if (strcmp(ext, "txt") == 0 || strcmp(ext, "ascii") == 0) {
            resp->body = (uint8_t *)ct_api_generate_ascii(ctx, z, x, y,
                                                           req->query, &resp->body_len);
            if (resp->body) {
                resp->status_code = 200;
                resp->content_type = "text/plain; charset=utf-8";
            } else {
                return sh_api_response_error(resp, 500, "ASCII tile generation failed");
            }
        } else {
            return sh_api_response_error(resp, 400, "Unknown tile format. Use .png, .mvt, or .txt");
        }

        return 0;
    }

    /* 404 for unknown paths */
    return sh_api_response_error(resp, 404, "Not found");
}
