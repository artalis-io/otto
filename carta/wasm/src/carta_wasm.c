/**
 * Carta WASM Entry Points
 *
 * Provides browser-friendly API for the Carta tile generator.
 */

#include <stdlib.h>
#include <string.h>
#include "carta.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

// =============================================================================
// Memory Management
// =============================================================================

WASM_EXPORT
void* wasm_malloc(int size) {
    return malloc(size);
}

WASM_EXPORT
void wasm_free(void* ptr) {
    free(ptr);
}

// =============================================================================
// Version
// =============================================================================

WASM_EXPORT
int wasm_version(void) {
    return CT_VERSION_MAJOR * 10000 + CT_VERSION_MINOR * 100 + CT_VERSION_PATCH;
}

// =============================================================================
// PBF Loading
// =============================================================================

/**
 * Load PBF from memory buffer
 * Returns context pointer or NULL on error
 */
WASM_EXPORT
CTPBFContext* wasm_load_pbf_memory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return NULL;
    return ct_load_pbf_memory(data, size);
}

WASM_EXPORT
void wasm_context_free(CTPBFContext* ctx) {
    if (ctx) {
        ct_free_pbf_context(ctx);
    }
}

WASM_EXPORT
uint32_t wasm_context_node_count(const CTPBFContext* ctx) {
    if (!ctx) return 0;
    CTPBFStats stats;
    ct_get_pbf_stats(ctx, &stats);
    return stats.node_count;
}

WASM_EXPORT
uint32_t wasm_context_way_count(const CTPBFContext* ctx) {
    if (!ctx) return 0;
    CTPBFStats stats;
    ct_get_pbf_stats(ctx, &stats);
    return stats.way_count;
}

/**
 * Get bounding box of loaded PBF
 * bbox array: [min_lat, min_lon, max_lat, max_lon]
 */
WASM_EXPORT
void wasm_context_bbox(const CTPBFContext* ctx, double* bbox) {
    if (!ctx || !bbox) return;

    CTPBFStats stats;
    ct_get_pbf_stats(ctx, &stats);

    bbox[0] = stats.bbox.min_lat;
    bbox[1] = stats.bbox.min_lon;
    bbox[2] = stats.bbox.max_lat;
    bbox[3] = stats.bbox.max_lon;
}

// =============================================================================
// Tile Generation
// =============================================================================

/**
 * Generate MVT vector tile
 *
 * @param ctx       PBF context
 * @param z         Zoom level
 * @param x         Tile X coordinate
 * @param y         Tile Y coordinate
 * @param buffer    Output buffer
 * @param capacity  Buffer capacity
 * @return Size of generated MVT, or 0 on error
 */
WASM_EXPORT
size_t wasm_generate_mvt(CTPBFContext* ctx,
                         int z, int x, int y,
                         uint8_t* buffer, size_t capacity) {
    if (!ctx || !buffer || capacity == 0) return 0;

    CTTileCoord coord = {(uint8_t)z, (uint32_t)x, (uint32_t)y};

    return ct_generate_mvt(ctx, coord, NULL, NULL, buffer, capacity);
}

/**
 * Generate PNG raster tile
 *
 * @param ctx       PBF context
 * @param z         Zoom level
 * @param x         Tile X coordinate
 * @param y         Tile Y coordinate
 * @param size      Tile size in pixels (e.g., 256 or 512)
 * @param buffer    Output buffer
 * @param capacity  Buffer capacity
 * @return Size of generated PNG, or 0 on error
 */
WASM_EXPORT
size_t wasm_generate_png(CTPBFContext* ctx,
                         int z, int x, int y, int size,
                         uint8_t* buffer, size_t capacity) {
    if (!ctx || !buffer || capacity == 0) return 0;

    CTTileCoord coord = {(uint8_t)z, (uint32_t)x, (uint32_t)y};
    CTStyle style = ct_default_style();

    return ct_generate_png(ctx, coord, &style, size, buffer, capacity);
}

// =============================================================================
// Utilities
// =============================================================================

/**
 * Get tile bounds in lat/lon
 * bounds array: [min_lat, min_lon, max_lat, max_lon]
 */
WASM_EXPORT
void wasm_tile_bounds(int z, int x, int y, double* bounds) {
    if (!bounds) return;

    CTTileCoord coord = {(uint8_t)z, (uint32_t)x, (uint32_t)y};
    CTBBox bbox = ct_tile_bounds(coord);

    bounds[0] = bbox.min_lat;
    bounds[1] = bbox.min_lon;
    bounds[2] = bbox.max_lat;
    bounds[3] = bbox.max_lon;
}
