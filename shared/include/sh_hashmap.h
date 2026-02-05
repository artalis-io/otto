/*
 * sh_hashmap.h - Generic hashmap implementation
 *
 * Provides a flexible hashmap with support for:
 * - int64_t keys (OSM IDs) to uint32_t/size_t values
 * - Open addressing with linear probing (memory efficient)
 * - Auto-resize at configurable load factor
 * - Zero sentinel for empty slots
 *
 * Used by carta (node_map, way_map) and velo (VLNodeMap) for
 * OSM ID to index mapping during PBF parsing.
 */

#ifndef SH_HASHMAP_H
#define SH_HASHMAP_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_HASHMAP_OK = 0,
    SH_HASHMAP_ERROR_NULL_PARAM,
    SH_HASHMAP_ERROR_OUT_OF_MEMORY,
    SH_HASHMAP_ERROR_KEY_NOT_FOUND,
    SH_HASHMAP_ERROR_INVALID_KEY,    /* Reserved key (e.g., 0) */
    SH_HASHMAP_ERROR_FULL            /* Failed to resize */
} SHHashmapStatus;

/* ============================================================================
 * Hashmap for int64 keys -> size_t values (open addressing)
 *
 * Uses parallel arrays for keys and values with linear probing.
 * Key value 0 is reserved as the empty sentinel.
 * ============================================================================ */

typedef struct {
    int64_t *keys;      /* Parallel array of keys (0 = empty slot) */
    size_t *values;     /* Parallel array of values */
    size_t capacity;    /* Total slots */
    size_t count;       /* Number of entries */
    float load_factor;  /* Resize threshold (default 0.75) */
} SHHashmapI64;

/*
 * Create a new hashmap with expected initial capacity.
 * Actual capacity will be at least expected_size / load_factor.
 */
SHHashmapI64 *sh_hashmap_i64_create(size_t expected_size);

/*
 * Create with custom load factor (0.5 - 0.9 recommended).
 */
SHHashmapI64 *sh_hashmap_i64_create_ex(size_t expected_size, float load_factor);

/*
 * Free a hashmap and all its memory.
 */
void sh_hashmap_i64_free(SHHashmapI64 *map);

/*
 * Clear all entries without freeing memory (reuse for next batch).
 */
void sh_hashmap_i64_clear(SHHashmapI64 *map);

/*
 * Insert or update a key-value pair.
 * Note: Key 0 is reserved and will return SH_HASHMAP_ERROR_INVALID_KEY.
 */
SHHashmapStatus sh_hashmap_i64_insert(SHHashmapI64 *map, int64_t key, size_t value);

/*
 * Look up a key and return its value.
 * Returns SIZE_MAX if key not found.
 */
size_t sh_hashmap_i64_lookup(const SHHashmapI64 *map, int64_t key);

/*
 * Check if a key exists in the map.
 */
int sh_hashmap_i64_contains(const SHHashmapI64 *map, int64_t key);

/*
 * Get current entry count.
 */
size_t sh_hashmap_i64_count(const SHHashmapI64 *map);

/*
 * Get current capacity (for debugging/stats).
 */
size_t sh_hashmap_i64_capacity(const SHHashmapI64 *map);

/* ============================================================================
 * Hashmap for int64 keys -> uint32_t values (space-optimized)
 *
 * Same as SHHashmapI64 but uses uint32_t values to save memory
 * when indices fit in 32 bits (common for node indices).
 * ============================================================================ */

typedef struct {
    int64_t *keys;       /* Parallel array of keys (0 = empty slot) */
    uint32_t *values;    /* Parallel array of 32-bit values */
    size_t capacity;     /* Total slots */
    size_t count;        /* Number of entries */
    float load_factor;   /* Resize threshold (default 0.75) */
} SHHashmapI64U32;

SHHashmapI64U32 *sh_hashmap_i64u32_create(size_t expected_size);
SHHashmapI64U32 *sh_hashmap_i64u32_create_ex(size_t expected_size, float load_factor);
void sh_hashmap_i64u32_free(SHHashmapI64U32 *map);
void sh_hashmap_i64u32_clear(SHHashmapI64U32 *map);
SHHashmapStatus sh_hashmap_i64u32_insert(SHHashmapI64U32 *map, int64_t key, uint32_t value);
uint32_t sh_hashmap_i64u32_lookup(const SHHashmapI64U32 *map, int64_t key);
int sh_hashmap_i64u32_contains(const SHHashmapI64U32 *map, int64_t key);
size_t sh_hashmap_i64u32_count(const SHHashmapI64U32 *map);
size_t sh_hashmap_i64u32_capacity(const SHHashmapI64U32 *map);

/* ============================================================================
 * Hash Function (exposed for use by other modules)
 * ============================================================================ */

/*
 * SplitMix64-based hash for int64 keys.
 * High-quality mixing suitable for open addressing.
 */
uint64_t sh_hash_i64(int64_t key);

/* ============================================================================
 * Iterator Support
 * ============================================================================ */

typedef struct {
    const SHHashmapI64 *map;
    size_t index;
} SHHashmapI64Iter;

typedef struct {
    const SHHashmapI64U32 *map;
    size_t index;
} SHHashmapI64U32Iter;

/*
 * Initialize an iterator for a hashmap.
 */
void sh_hashmap_i64_iter_init(SHHashmapI64Iter *iter, const SHHashmapI64 *map);
void sh_hashmap_i64u32_iter_init(SHHashmapI64U32Iter *iter, const SHHashmapI64U32 *map);

/*
 * Get the next key-value pair from the iterator.
 * Returns 1 if a pair was found, 0 if iteration is complete.
 */
int sh_hashmap_i64_iter_next(SHHashmapI64Iter *iter, int64_t *key, size_t *value);
int sh_hashmap_i64u32_iter_next(SHHashmapI64U32Iter *iter, int64_t *key, uint32_t *value);

#endif /* SH_HASHMAP_H */
