/*
 * ct_cache.c - Tile cache implementation
 *
 * Uses hash table with chaining for O(1) average lookup.
 * LRU eviction when cache is full.
 */

#include "ct_cache.h"
#include "shared.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ============================================================================
 * Hash Function
 * ============================================================================ */

static uint64_t hash_key(uint64_t key)
{
    /* Mix bits for better distribution */
    key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ULL;
    key = (key ^ (key >> 27)) * 0x94d049bb133111ebULL;
    key = key ^ (key >> 31);
    return key;
}

/* ============================================================================
 * Entry Management
 * ============================================================================ */

static CTCacheEntry *entry_create(uint64_t key, const uint8_t *data, size_t size)
{
    CTCacheEntry *entry = malloc(sizeof(CTCacheEntry));
    if (!entry) return NULL;

    entry->data = malloc(size);
    if (!entry->data) {
        free(entry);
        return NULL;
    }

    memcpy(entry->data, data, size);
    entry->key = key;
    entry->size = size;
    entry->last_access = 0;
    entry->next = NULL;

    return entry;
}

static void entry_free(CTCacheEntry *entry)
{
    if (!entry) return;
    SAFE_FREE(entry->data);
    free(entry);
}

/* ============================================================================
 * Cache Creation/Destruction
 * ============================================================================ */

CTTileCache *ct_cache_create(size_t max_mb)
{
    CTTileCache *cache = calloc(1, sizeof(CTTileCache));
    if (!cache) return NULL;

    /* Default to 256MB if not specified */
    if (max_mb == 0) max_mb = CT_CACHE_DEFAULT_MB;
    cache->max_bytes = max_mb * 1024 * 1024;

    /* Size hash table for reasonable load factor */
    /* Assume average tile ~10KB, so max_mb * 100 entries roughly */
    cache->num_buckets = (max_mb * 100) | 1;  /* Make odd for better hashing */
    if (cache->num_buckets < 1009) cache->num_buckets = 1009;  /* Minimum */

    cache->buckets = calloc(cache->num_buckets, sizeof(CTCacheEntry *));
    if (!cache->buckets) {
        free(cache);
        return NULL;
    }

    return cache;
}

void ct_cache_free(CTTileCache *cache)
{
    if (!cache) return;

    /* Free all entries */
    for (size_t i = 0; i < cache->num_buckets; i++) {
        CTCacheEntry *entry = cache->buckets[i];
        while (entry) {
            CTCacheEntry *next = entry->next;
            entry_free(entry);
            entry = next;
        }
    }

    free(cache->buckets);
    free(cache);
}

/* ============================================================================
 * Lookup
 * ============================================================================ */

bool ct_cache_get(CTTileCache *cache, int z, int x, int y,
                  const uint8_t **data, size_t *size)
{
    if (!cache || z < 0 || z >= CT_CACHE_MAX_ZOOM) return false;

    uint64_t key = CT_CACHE_KEY(z, x, y);
    uint64_t bucket = hash_key(key) % cache->num_buckets;

    CTCacheEntry *entry = cache->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            /* Cache hit */
            entry->last_access = ++cache->clock;
            cache->hits++;
            *data = entry->data;
            *size = entry->size;
            return true;
        }
        entry = entry->next;
    }

    /* Cache miss */
    cache->misses++;
    return false;
}

/* ============================================================================
 * Eviction
 * ============================================================================ */

/*
 * Find and remove the least recently used entry.
 * Returns bytes freed.
 */
static size_t evict_lru(CTTileCache *cache)
{
    CTCacheEntry *oldest = NULL;
    CTCacheEntry **oldest_prev = NULL;
    uint32_t oldest_time = UINT32_MAX;

    /* Find LRU entry */
    for (size_t i = 0; i < cache->num_buckets; i++) {
        CTCacheEntry **prev = &cache->buckets[i];
        CTCacheEntry *entry = *prev;

        while (entry) {
            if (entry->last_access < oldest_time) {
                oldest_time = entry->last_access;
                oldest = entry;
                oldest_prev = prev;
            }
            prev = &entry->next;
            entry = entry->next;
        }
    }

    if (!oldest) return 0;

    /* Remove from chain */
    *oldest_prev = oldest->next;

    /* Update stats */
    int z = CT_CACHE_KEY_Z(oldest->key);
    if (z >= 0 && z < CT_CACHE_MAX_ZOOM) {
        cache->zoom_stats[z].count--;
        cache->zoom_stats[z].bytes -= oldest->size;
    }
    cache->total_entries--;
    size_t freed = oldest->size;
    cache->total_bytes -= freed;

    entry_free(oldest);
    return freed;
}

/* ============================================================================
 * Store
 * ============================================================================ */

bool ct_cache_put(CTTileCache *cache, int z, int x, int y,
                  const uint8_t *data, size_t size)
{
    if (!cache || !data || size == 0) return false;
    if (z < 0 || z >= CT_CACHE_MAX_ZOOM) return false;

    /* Don't cache if single tile exceeds budget */
    if (size > cache->max_bytes / 2) return false;

    uint64_t key = CT_CACHE_KEY(z, x, y);
    uint64_t bucket = hash_key(key) % cache->num_buckets;

    /* Check if already cached */
    CTCacheEntry *entry = cache->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            /* Already cached, update access time */
            entry->last_access = ++cache->clock;
            return true;
        }
        entry = entry->next;
    }

    /* Evict until we have space */
    while (cache->total_bytes + size > cache->max_bytes) {
        if (evict_lru(cache) == 0) {
            /* Can't evict anything */
            return false;
        }
    }

    /* Create new entry */
    entry = entry_create(key, data, size);
    if (!entry) return false;

    entry->last_access = ++cache->clock;

    /* Add to hash chain */
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;

    /* Update stats */
    cache->zoom_stats[z].count++;
    cache->zoom_stats[z].bytes += size;
    cache->total_entries++;
    cache->total_bytes += size;

    return true;
}

/* ============================================================================
 * Clear
 * ============================================================================ */

void ct_cache_clear(CTTileCache *cache)
{
    if (!cache) return;

    for (size_t i = 0; i < cache->num_buckets; i++) {
        CTCacheEntry *entry = cache->buckets[i];
        while (entry) {
            CTCacheEntry *next = entry->next;
            entry_free(entry);
            entry = next;
        }
        cache->buckets[i] = NULL;
    }

    memset(cache->zoom_stats, 0, sizeof(cache->zoom_stats));
    cache->total_entries = 0;
    cache->total_bytes = 0;
    /* Preserve hits/misses for diagnostics */
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

void ct_cache_stats(const CTTileCache *cache,
                    size_t *total_entries, size_t *total_bytes,
                    uint64_t *hits, uint64_t *misses)
{
    if (!cache) return;
    if (total_entries) *total_entries = cache->total_entries;
    if (total_bytes) *total_bytes = cache->total_bytes;
    if (hits) *hits = cache->hits;
    if (misses) *misses = cache->misses;
}
