/*
 * ct_metatile.h - Metatile Label Placement Cache
 *
 * Places labels across a 2x2 tile group (metatile) using a shared collision
 * grid, then caches the result.  Sub-tiles extract their portion from the
 * cached metatile so labels are consistent across tile boundaries.
 *
 * The cache is thread-safe (pthread_rwlock) and uses LRU eviction.
 */

#ifndef CT_METATILE_H
#define CT_METATILE_H

#include "ct_types.h"
#include "ct_label.h"
#include "ct_pbf.h"
#include "sh_font.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CT_METATILE_SIZE  2   /* 2x2 tile groups */

/* Default cache capacity (number of metatile entries) */
#define CT_METATILE_CACHE_DEFAULT  4096

/* ============================================================================
 * Types
 * ============================================================================ */

/* Metatile coordinate: identifies a 2x2 group of tiles */
typedef struct {
    int z;    /* Zoom level */
    int mx;   /* tile_x & ~1 (even-aligned x) */
    int my;   /* tile_y & ~1 (even-aligned y) */
} CTMetatileCoord;

/* Cached label placement result for a metatile */
typedef struct {
    CTMetatileCoord coord;
    int tile_size;

    /* Point/area labels in metatile pixel coords (0..tile_size*2) */
    CTLabelPlacement *labels;
    size_t num_labels;

    /* Road labels with glyph coords in metatile pixel space */
    CTRoadLabelPlacement *roads;
    size_t num_roads;

    int refcount;   /* For safe eviction while in use */
} CTMetatileLabelResult;

/* Opaque cache type */
typedef struct CTMetatileLabelCache CTMetatileLabelCache;

/* ============================================================================
 * Cache Lifecycle
 * ============================================================================ */

/*
 * Create a metatile label cache.
 *
 * @param max_entries  Maximum number of cached metatiles (0 = disabled)
 * @return             New cache, or NULL on failure
 */
CTMetatileLabelCache *ct_metatile_cache_create(size_t max_entries);

/*
 * Free a metatile label cache and all cached results.
 */
void ct_metatile_cache_free(CTMetatileLabelCache *cache);

/* ============================================================================
 * Cache Operations
 * ============================================================================ */

/*
 * Look up a metatile result in the cache.
 *
 * On hit, increments the refcount.  Caller MUST call ct_metatile_cache_release()
 * when done with the result.
 *
 * @param cache  Label cache
 * @param mt     Metatile coordinate
 * @return       Cached result (refcounted), or NULL on miss
 */
const CTMetatileLabelResult *ct_metatile_cache_get(
    CTMetatileLabelCache *cache, CTMetatileCoord mt);

/*
 * Release a refcount obtained from ct_metatile_cache_get().
 */
void ct_metatile_cache_release(
    CTMetatileLabelCache *cache, const CTMetatileLabelResult *result);

/*
 * Insert a computed metatile result into the cache.
 * Takes ownership of the result.  May evict LRU entries.
 *
 * @param cache   Label cache
 * @param mt      Metatile coordinate
 * @param result  Computed result (cache takes ownership)
 */
void ct_metatile_cache_put(
    CTMetatileLabelCache *cache, CTMetatileCoord mt,
    CTMetatileLabelResult *result);

/* ============================================================================
 * Metatile Computation
 * ============================================================================ */

/*
 * Compute label placements across a 2x2 metatile group.
 *
 * Queries labeled points, area labels, and road labels from all 4 sub-tiles,
 * deduplicates by pointer identity, and places them using a shared collision
 * grid sized (tile_size*2) x (tile_size*2).
 *
 * @param pbf        PBF context with parsed data
 * @param mt         Metatile coordinate (z, mx, my)
 * @param tile_size  Single tile size in pixels (e.g. 512)
 * @return           New result (caller owns), or NULL on failure
 */
CTMetatileLabelResult *ct_metatile_compute_labels(
    const CTPBFContext *pbf, CTMetatileCoord mt, int tile_size);

/*
 * Extract labels for a single sub-tile from a metatile result.
 *
 * Translates coordinates from metatile space to sub-tile space and
 * populates a CTLabelPlacer with the labels that fall within the sub-tile.
 *
 * @param result      Metatile label result
 * @param sx, sy      Sub-tile position within metatile (0 or 1)
 * @param placer      Label placer to populate (must be created for tile_size)
 * @param out_roads   Output: road label placements for sub-tile (caller owns)
 * @param out_road_count  Output: number of road placements
 */
void ct_metatile_extract_subtile(
    const CTMetatileLabelResult *result,
    int sx, int sy,
    CTLabelPlacer *placer,
    CTRoadLabelPlacement **out_roads, size_t *out_road_count);

/*
 * Free a metatile label result.
 */
void ct_metatile_result_free(CTMetatileLabelResult *result);

/* ============================================================================
 * Coordinate Helpers
 * ============================================================================ */

/*
 * Compute metatile coordinate from a tile coordinate.
 */
static inline CTMetatileCoord ct_metatile_coord(CTTileCoord coord)
{
    CTMetatileCoord mt;
    mt.z = coord.z;
    mt.mx = coord.x & ~1;
    mt.my = coord.y & ~1;
    return mt;
}

/*
 * Get sub-tile position within a metatile (0 or 1 for each axis).
 */
static inline void ct_metatile_subtile(CTTileCoord coord, int *sx, int *sy)
{
    *sx = coord.x & 1;
    *sy = coord.y & 1;
}

#ifdef __cplusplus
}
#endif

#endif /* CT_METATILE_H */
