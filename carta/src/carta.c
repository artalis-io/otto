/*
 * carta.c - Carta main API implementation
 *
 * High-level functions for loading OSM data and generating tiles.
 */

#include "carta.h"
#include "ct_boundary.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARTA_VERSION "0.1.0"

/* ============================================================================
 * Version and Info
 * ============================================================================ */

const char *ct_version(void)
{
    return CARTA_VERSION;
}

int ct_mvt_spec_version(void)
{
    return 2;  /* MVT spec version 2.1 */
}

const char *ct_status_string(CTStatus status)
{
    switch (status) {
        case CT_OK:                     return "OK";
        case CT_ERROR_INVALID_ARGUMENT: return "Invalid argument";
        case CT_ERROR_OUT_OF_MEMORY:    return "Out of memory";
        case CT_ERROR_FILE_NOT_FOUND:   return "File not found";
        case CT_ERROR_FILE_READ:        return "File read error";
        case CT_ERROR_FILE_WRITE:       return "File write error";
        case CT_ERROR_PARSE_ERROR:      return "Parse error";
        case CT_ERROR_BUFFER_TOO_SMALL: return "Buffer too small";
        case CT_ERROR_UNSUPPORTED_FORMAT: return "Unsupported format";
        case CT_ERROR_INTERNAL:         return "Internal error";
        default:                        return "Unknown error";
    }
}

/* ============================================================================
 * High-Level Loading
 * ============================================================================ */

CTPBFContext *ct_load_pbf_with_config(const char *filename, const CTPBFConfig *config)
{
    if (!filename) return NULL;

    /* Check if it's a binary index file (fast path) */
    if (ct_is_binary_index(filename)) {
        return ct_index_mmap(filename);
    }

    /* Parse PBF file (slow path) */
    CTPBFContext *ctx = ct_pbf_context_create_with_config(config);
    if (!ctx) return NULL;

    CTStatus status = ct_pbf_parse_file(ctx, filename);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for ways */
    status = ct_pbf_build_index(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Assemble multipolygon relations into renderable geometries */
    status = ct_assemble_multipolygons(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for multipolygons */
    status = ct_build_multipolygon_rtree(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Assemble boundary relations into continuous linestrings */
    status = ct_assemble_boundaries(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for boundaries */
    status = ct_build_boundary_rtree(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    return ctx;
}

CTPBFContext *ct_load_pbf(const char *filename)
{
    return ct_load_pbf_with_config(filename, NULL);
}

CTPBFContext *ct_load_pbf_memory(const uint8_t *data, size_t size)
{
    if (!data || size == 0) return NULL;

    CTPBFContext *ctx = ct_pbf_context_create();
    if (!ctx) return NULL;

    CTStatus status = ct_pbf_parse_memory(ctx, data, size);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for ways */
    status = ct_pbf_build_index(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Assemble multipolygon relations into renderable geometries */
    status = ct_assemble_multipolygons(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for multipolygons */
    status = ct_build_multipolygon_rtree(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Assemble boundary relations into continuous linestrings */
    status = ct_assemble_boundaries(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    /* Build spatial index for boundaries */
    status = ct_build_boundary_rtree(ctx);
    if (status != CT_OK) {
        ct_pbf_context_free(ctx);
        return NULL;
    }

    return ctx;
}

void ct_free_pbf_context(CTPBFContext *ctx)
{
    ct_pbf_context_free(ctx);
}

/* ============================================================================
 * Batch Tile Generation
 * ============================================================================ */

void ct_generate_tiles(const CTPBFContext *ctx, const CTBBox *bbox,
                       int min_zoom, int max_zoom, int vector,
                       CTTileCallback callback, void *user_data)
{
    if (!ctx || !callback) return;
    if (min_zoom < 0) min_zoom = 0;
    if (max_zoom > CT_MAX_ZOOM) max_zoom = CT_MAX_ZOOM;

    /* Use loaded data bbox if not specified */
    CTBBox bounds;
    if (bbox) {
        bounds = *bbox;
    } else {
        bounds = ctx->bbox;
    }

    /* Allocate buffer for tile data */
    size_t buffer_size = vector ? (1024 * 1024) : (512 * 1024);
    uint8_t *buffer = malloc(buffer_size);
    if (!buffer) return;

    CTMVTOptions mvt_opts;
    ct_mvt_default_options(&mvt_opts);

    CTStyle style;
    ct_default_style(&style);

    CTPNGOptions png_opts;
    ct_png_default_options(&png_opts);

    /* Generate tiles for each zoom level */
    for (int z = min_zoom; z <= max_zoom; z++) {
        CTTileCoord *tiles = NULL;
        int num_tiles = ct_tiles_for_bbox(bounds, z, &tiles);

        for (int i = 0; i < num_tiles; i++) {
            size_t size;

            if (vector) {
                size = ct_generate_mvt(ctx, tiles[i], &mvt_opts, NULL,
                                       buffer, buffer_size);
            } else {
                size = ct_generate_png(ctx, tiles[i], &style, &png_opts,
                                       buffer, buffer_size);
            }

            if (size > 0) {
                callback(tiles[i], buffer, size, user_data);
            }
        }

        free(tiles);
    }

    free(buffer);
}

/* ============================================================================
 * TileJSON Generation
 * ============================================================================ */

size_t ct_generate_tilejson(const CTPBFContext *ctx, const char *name,
                            int min_zoom, int max_zoom,
                            char *buffer, size_t capacity)
{
    if (!ctx || !buffer || capacity == 0) return 0;

    const char *fmt =
        "{\n"
        "  \"tilejson\": \"2.2.0\",\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Generated by Carta\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"minzoom\": %d,\n"
        "  \"maxzoom\": %d,\n"
        "  \"bounds\": [%.6f, %.6f, %.6f, %.6f],\n"
        "  \"center\": [%.6f, %.6f, %d],\n"
        "  \"vector_layers\": [\n"
        "    {\"id\": \"roads\", \"description\": \"Road network\"},\n"
        "    {\"id\": \"water\", \"description\": \"Water features\"},\n"
        "    {\"id\": \"buildings\", \"description\": \"Buildings\"},\n"
        "    {\"id\": \"landuse\", \"description\": \"Land use areas\"}\n"
        "  ]\n"
        "}\n";

    double center_lon = (ctx->bbox.min_lon + ctx->bbox.max_lon) / 2;
    double center_lat = (ctx->bbox.min_lat + ctx->bbox.max_lat) / 2;
    int center_zoom = (min_zoom + max_zoom) / 2;

    int written = snprintf(buffer, capacity, fmt,
                           name ? name : "carta",
                           min_zoom, max_zoom,
                           ctx->bbox.min_lon, ctx->bbox.min_lat,
                           ctx->bbox.max_lon, ctx->bbox.max_lat,
                           center_lon, center_lat, center_zoom);

    return (written > 0 && (size_t)written < capacity) ? (size_t)written : 0;
}
