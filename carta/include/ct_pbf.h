/*
 * ct_pbf.h - OSM PBF file parsing for map features
 *
 * Parses OpenStreetMap Protocol Buffer Format files, extracting
 * features needed for map rendering (roads, water, buildings, etc.).
 */

#ifndef CT_PBF_H
#define CT_PBF_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * PBF Context Management
 * ============================================================================ */

/*
 * Create a new PBF parsing context.
 */
CTPBFContext *ct_pbf_context_create(void);

/*
 * Free a PBF parsing context and all associated data.
 */
void ct_pbf_context_free(CTPBFContext *ctx);

/* ============================================================================
 * PBF Parsing
 * ============================================================================ */

/*
 * Parse an OSM PBF file from disk.
 *
 * @param ctx      Parsing context (created with ct_pbf_context_create)
 * @param filename Path to .osm.pbf file
 * @return CT_OK on success
 */
CTStatus ct_pbf_parse_file(CTPBFContext *ctx, const char *filename);

/*
 * Parse an OSM PBF file from memory.
 * Useful for WASM where file I/O is not available.
 *
 * @param ctx  Parsing context
 * @param data PBF file data
 * @param size Size of data in bytes
 * @return CT_OK on success
 */
CTStatus ct_pbf_parse_memory(CTPBFContext *ctx, const uint8_t *data, size_t size);

/* ============================================================================
 * Spatial Queries
 * ============================================================================ */

/*
 * Build spatial index for fast tile queries.
 * Call this after parsing, before generating tiles.
 *
 * @param ctx Parsing context with loaded data
 * @return CT_OK on success
 */
CTStatus ct_pbf_build_index(CTPBFContext *ctx);

/*
 * Get all features that intersect a tile.
 *
 * @param ctx      Parsing context with spatial index
 * @param tile     Tile coordinates
 * @param features Output feature array (caller frees)
 * @param count    Output feature count
 * @return CT_OK on success
 */
CTStatus ct_pbf_get_tile_features(const CTPBFContext *ctx, CTTileCoord tile,
                                  CTFeature **features, size_t *count);

/*
 * Get all features that intersect a bounding box.
 *
 * @param ctx      Parsing context with spatial index
 * @param bbox     Geographic bounds
 * @param features Output feature array (caller frees)
 * @param count    Output feature count
 * @return CT_OK on success
 */
CTStatus ct_pbf_get_bbox_features(const CTPBFContext *ctx, CTBBox bbox,
                                  CTFeature **features, size_t *count);

/* Forward declaration for LOD config */
struct CTLODConfig;

/*
 * Get features for a tile with LOD filtering.
 *
 * Features are filtered based on the LOD configuration and zoom level.
 * This is the recommended function for tile generation.
 *
 * @param ctx      Parsing context with spatial index
 * @param coord    Tile coordinates (includes zoom level)
 * @param lod      LOD configuration (NULL = no filtering)
 * @param features Output feature array (caller frees)
 * @param count    Output feature count
 * @return CT_OK on success
 */
CTStatus ct_pbf_get_tile_features_lod(const CTPBFContext *ctx, CTTileCoord coord,
                                      const struct CTLODConfig *lod,
                                      CTFeature **features, size_t *count);

/* ============================================================================
 * Statistics
 * ============================================================================ */

/*
 * Get parsing statistics.
 */
void ct_pbf_stats(const CTPBFContext *ctx,
                  size_t *total_nodes,
                  size_t *total_ways,
                  size_t *features_kept,
                  CTBBox *bbox);

#ifdef __cplusplus
}
#endif

#endif /* CT_PBF_H */
