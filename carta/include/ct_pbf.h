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
 * Configuration
 * ============================================================================ */

/*
 * Progress callback for long-running operations.
 *
 * @param phase     Current phase ("parsing", "indexing", etc.)
 * @param current   Current progress value
 * @param total     Total expected value (0 if unknown)
 * @param user_data User-provided context pointer
 */
typedef void (*CTPBFProgressCallback)(const char *phase, size_t current,
                                       size_t total, void *user_data);

/*
 * Configuration for PBF parsing.
 * All fields have sensible defaults if set to 0.
 */
typedef struct {
    /* Memory limits (0 = use defaults) */
    size_t max_node_capacity;    /* Default: 256M, env: CARTA_MAX_NODES */
    size_t max_way_capacity;     /* Default: 64M,  env: CARTA_MAX_WAYS */
    size_t max_coord_capacity;   /* Default: 256M, env: CARTA_MAX_COORDS */
    size_t arena_size;           /* Default: 128M, env: CARTA_ARENA_SIZE */

    /* Progress reporting */
    CTPBFProgressCallback progress_callback;
    void *progress_user_data;
    size_t progress_interval;    /* Report every N blobs (0 = every blob) */
} CTPBFConfig;

/*
 * Initialize config with defaults.
 * Reads from environment variables if set.
 */
void ct_pbf_config_init(CTPBFConfig *config);

/* ============================================================================
 * PBF Context Management
 * ============================================================================ */

/*
 * Create a new PBF parsing context with default configuration.
 * Reads memory limits from environment variables if set.
 *
 * OWNERSHIP: Caller owns the returned context and must call
 * ct_pbf_context_free() when done.
 *
 * @return New context, or NULL on allocation failure
 */
CTPBFContext *ct_pbf_context_create(void);

/*
 * Create a new PBF parsing context with custom configuration.
 *
 * OWNERSHIP: Caller owns the returned context and must call
 * ct_pbf_context_free() when done.
 *
 * @param config Configuration (NULL = use defaults)
 * @return New context, or NULL on allocation failure
 */
CTPBFContext *ct_pbf_context_create_with_config(const CTPBFConfig *config);

/*
 * Free a PBF parsing context and all associated data.
 * Safe to call with NULL (no-op).
 *
 * @param ctx Context to free (may be NULL)
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
 * OWNERSHIP: Caller owns the returned *features array and each feature's
 * internal arrays (points, ring_ends). Free with:
 *   for (i = 0; i < count; i++) { free(features[i].points); }
 *   free(features);
 *
 * @param ctx      Parsing context with spatial index
 * @param tile     Tile coordinates
 * @param features Output feature array (caller owns)
 * @param count    Output feature count
 * @return CT_OK on success
 */
CTStatus ct_pbf_get_tile_features(const CTPBFContext *ctx, CTTileCoord tile,
                                  CTFeature **features, size_t *count);

/*
 * Get all features that intersect a bounding box.
 *
 * OWNERSHIP: Caller owns the returned *features array and each feature's
 * internal arrays (points, ring_ends). Free with:
 *   for (i = 0; i < count; i++) { free(features[i].points); }
 *   free(features);
 *
 * @param ctx      Parsing context with spatial index
 * @param bbox     Geographic bounds
 * @param features Output feature array (caller owns)
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
 * OWNERSHIP: Caller owns the returned *features array and each feature's
 * internal arrays (points, ring_ends). See ct_pbf_get_tile_features().
 *
 * @param ctx      Parsing context with spatial index
 * @param coord    Tile coordinates (includes zoom level)
 * @param lod      LOD configuration (NULL = no filtering)
 * @param features Output feature array (caller owns)
 * @param count    Output feature count
 * @return CT_OK on success
 */
CTStatus ct_pbf_get_tile_features_lod(const CTPBFContext *ctx, CTTileCoord coord,
                                      const struct CTLODConfig *lod,
                                      CTFeature **features, size_t *count);

/* ============================================================================
 * Labeled Points (for map labels)
 * ============================================================================ */

/*
 * Get labeled points (cities, towns, etc.) that intersect a tile.
 *
 * @param ctx       Parsing context
 * @param coord     Tile coordinates (includes zoom level for filtering)
 * @param points    Output: array of pointers to labeled points (caller frees array, not contents)
 * @param count     Output: number of points
 * @return CT_OK on success
 *
 * Note: The returned pointers reference data owned by ctx. Do not free the
 * CTLabeledPoint structures themselves, only the array.
 */
CTStatus ct_pbf_get_tile_labels(const CTPBFContext *ctx, CTTileCoord coord,
                                const CTLabeledPoint ***points, size_t *count);

/*
 * Get the number of labeled points parsed.
 */
size_t ct_pbf_get_label_count(const CTPBFContext *ctx);

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
