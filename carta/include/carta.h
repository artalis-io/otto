/*
 * carta.h - Carta Map Tile Generator
 *
 * Unified header for the Carta library.
 * Includes all public API for vector and raster tile generation.
 *
 * Carta: Compact Agile Rendering for Tile Archives
 */

#ifndef CARTA_H
#define CARTA_H

#include "ct_types.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_mvt.h"
#include "ct_render.h"
#include "ct_png.h"
#include "ct_lod.h"
#include "ct_simplify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * High-Level API
 * ============================================================================ */

/*
 * Load OSM data from a PBF file.
 * This parses the file and builds a spatial index for efficient tile queries.
 *
 * @param filename Path to .osm.pbf file
 * @return Context for tile generation, or NULL on error
 */
CTPBFContext *ct_load_pbf(const char *filename);

/*
 * Load OSM data from memory.
 * For WASM usage where file I/O is not available.
 *
 * @param data PBF file data
 * @param size Size in bytes
 * @return Context for tile generation, or NULL on error
 */
CTPBFContext *ct_load_pbf_memory(const uint8_t *data, size_t size);

/*
 * Free all resources associated with a loaded PBF.
 */
void ct_free_pbf_context(CTPBFContext *ctx);

/* ============================================================================
 * Batch Tile Generation
 * ============================================================================ */

/* Callback for batch tile generation */
typedef void (*CTTileCallback)(CTTileCoord coord, const uint8_t *data,
                               size_t size, void *user_data);

/*
 * Generate all tiles for a region.
 *
 * @param ctx       PBF context
 * @param bbox      Geographic bounds (NULL = entire loaded area)
 * @param min_zoom  Minimum zoom level
 * @param max_zoom  Maximum zoom level
 * @param vector    1 for MVT, 0 for PNG
 * @param callback  Called for each generated tile
 * @param user_data Passed to callback
 */
void ct_generate_tiles(const CTPBFContext *ctx, const CTBBox *bbox,
                       int min_zoom, int max_zoom, int vector,
                       CTTileCallback callback, void *user_data);

/* ============================================================================
 * TileJSON Metadata
 * ============================================================================ */

/*
 * Generate TileJSON metadata for a tileset.
 * TileJSON describes the tile source for map libraries.
 *
 * @param ctx       PBF context
 * @param name      Tileset name
 * @param min_zoom  Minimum zoom level
 * @param max_zoom  Maximum zoom level
 * @param buffer    Output buffer
 * @param capacity  Buffer size
 * @return Bytes written or 0 on error
 */
size_t ct_generate_tilejson(const CTPBFContext *ctx, const char *name,
                            int min_zoom, int max_zoom,
                            char *buffer, size_t capacity);

/* ============================================================================
 * Version and Info
 * ============================================================================ */

/*
 * Get Carta library version string.
 */
const char *ct_version(void);

/*
 * Get supported MVT specification version.
 */
int ct_mvt_spec_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CARTA_H */
