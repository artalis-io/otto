/*
 * ct_cache.h - Tile cache with LRU eviction
 *
 * In-memory cache for generated tiles. Separate caches per zoom level
 * for optimal memory distribution.
 */

#ifndef CT_CACHE_H
#define CT_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define CT_CACHE_MAX_ZOOM 19      /* z0-z18 */
#define CT_CACHE_DEFAULT_MB 256   /* Default cache size in MB */

/* ============================================================================
 * Tile Key
 * ============================================================================ */

/* Pack z/x/y into 64-bit key: z(5) | x(29) | y(30) */
#define CT_CACHE_KEY(z, x, y) \
    (((uint64_t)(z) << 59) | ((uint64_t)(x) << 30) | (uint64_t)(y))

#define CT_CACHE_KEY_Z(k) ((int)((k) >> 59))
#define CT_CACHE_KEY_X(k) ((int)(((k) >> 30) & 0x1FFFFFFF))
#define CT_CACHE_KEY_Y(k) ((int)((k) & 0x3FFFFFFF))

/* ============================================================================
 * Cache Entry
 * ============================================================================ */

typedef struct CTCacheEntry {
    uint64_t key;           /* Packed z/x/y */
    uint8_t *data;          /* Tile data (owned) */
    size_t size;            /* Data size in bytes */
    uint32_t last_access;   /* LRU timestamp */
    struct CTCacheEntry *next;  /* Hash chain */
} CTCacheEntry;

/* ============================================================================
 * Tile Cache
 * ============================================================================ */

typedef struct {
    /* Hash table for fast lookup */
    CTCacheEntry **buckets;
    size_t num_buckets;

    /* Statistics per zoom level */
    struct {
        size_t count;       /* Number of entries */
        size_t bytes;       /* Total bytes used */
    } zoom_stats[CT_CACHE_MAX_ZOOM];

    /* Global stats */
    size_t total_entries;
    size_t total_bytes;
    size_t max_bytes;       /* Cache budget */
    uint32_t clock;         /* LRU clock */

    /* Hit/miss stats */
    uint64_t hits;
    uint64_t misses;
} CTTileCache;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Create a new tile cache.
 *
 * @param max_mb  Maximum cache size in megabytes (0 = default 256MB)
 * @return        New cache, or NULL on failure
 */
CTTileCache *ct_cache_create(size_t max_mb);

/*
 * Free cache and all entries.
 */
void ct_cache_free(CTTileCache *cache);

/*
 * Look up a tile in the cache.
 *
 * @param cache   Cache to search
 * @param z, x, y Tile coordinates
 * @param data    Output: pointer to cached data (NOT owned by caller)
 * @param size    Output: size of cached data
 * @return        true if found (cache hit), false if not found (cache miss)
 */
bool ct_cache_get(CTTileCache *cache, int z, int x, int y,
                  const uint8_t **data, size_t *size);

/*
 * Store a tile in the cache.
 *
 * The cache takes ownership of the data copy (caller's data is copied).
 * May evict older entries if cache is full.
 *
 * @param cache   Cache to store in
 * @param z, x, y Tile coordinates
 * @param data    Tile data to cache (will be copied)
 * @param size    Size of tile data
 * @return        true on success, false on failure
 */
bool ct_cache_put(CTTileCache *cache, int z, int x, int y,
                  const uint8_t *data, size_t size);

/*
 * Clear all entries from the cache.
 */
void ct_cache_clear(CTTileCache *cache);

/*
 * Get cache statistics.
 */
void ct_cache_stats(const CTTileCache *cache,
                    size_t *total_entries, size_t *total_bytes,
                    uint64_t *hits, uint64_t *misses);

#ifdef __cplusplus
}
#endif

#endif /* CT_CACHE_H */
