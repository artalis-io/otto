/*
 * sh_hashmap.c - Generic hashmap implementation
 *
 * Uses open addressing with linear probing for memory efficiency.
 * Key 0 is reserved as the empty sentinel.
 */

#include "sh_hashmap.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define SH_HASHMAP_MIN_CAPACITY 64
#define SH_HASHMAP_DEFAULT_LOAD_FACTOR 0.75f
#define SH_HASHMAP_INVALID_U32 UINT32_MAX

/* ============================================================================
 * Hash Function
 * ============================================================================ */

/*
 * SplitMix64-based hash function.
 * Provides excellent avalanche properties for open addressing.
 */
uint64_t sh_hash_i64(int64_t key)
{
    uint64_t x = (uint64_t)key;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

/* ============================================================================
 * SHHashmapI64 Implementation (int64 -> size_t)
 * ============================================================================ */

static size_t next_power_of_two(size_t n)
{
    if (n == 0) return SH_HASHMAP_MIN_CAPACITY;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    n++;
    return n < SH_HASHMAP_MIN_CAPACITY ? SH_HASHMAP_MIN_CAPACITY : n;
}

SHHashmapI64 *sh_hashmap_i64_create(size_t expected_size)
{
    return sh_hashmap_i64_create_ex(expected_size, SH_HASHMAP_DEFAULT_LOAD_FACTOR);
}

SHHashmapI64 *sh_hashmap_i64_create_ex(size_t expected_size, float load_factor)
{
    if (load_factor < 0.1f) load_factor = 0.1f;
    if (load_factor > 0.95f) load_factor = 0.95f;

    SHHashmapI64 *map = malloc(sizeof(SHHashmapI64));
    if (!map) return NULL;

    /* Calculate capacity based on expected size and load factor */
    size_t min_capacity = (size_t)((float)expected_size / load_factor) + 1;
    size_t capacity = next_power_of_two(min_capacity);

    map->keys = calloc(capacity, sizeof(int64_t));
    map->values = calloc(capacity, sizeof(size_t));  /* Use calloc for overflow protection */
    if (!map->keys || !map->values) {
        free(map->keys);
        free(map->values);
        free(map);
        return NULL;
    }

    map->capacity = capacity;
    map->count = 0;
    map->load_factor = load_factor;

    return map;
}

void sh_hashmap_i64_free(SHHashmapI64 *map)
{
    if (!map) return;
    free(map->keys);
    free(map->values);
    free(map);
}

void sh_hashmap_i64_clear(SHHashmapI64 *map)
{
    if (!map) return;
    memset(map->keys, 0, map->capacity * sizeof(int64_t));
    map->count = 0;
}

static SHHashmapStatus sh_hashmap_i64_resize(SHHashmapI64 *map)
{
    size_t new_capacity = map->capacity * 2;
    if (new_capacity < map->capacity) {
        /* Overflow */
        return SH_HASHMAP_ERROR_OUT_OF_MEMORY;
    }

    int64_t *new_keys = calloc(new_capacity, sizeof(int64_t));
    size_t *new_values = calloc(new_capacity, sizeof(size_t));  /* Use calloc for overflow protection */
    if (!new_keys || !new_values) {
        free(new_keys);
        free(new_values);
        return SH_HASHMAP_ERROR_OUT_OF_MEMORY;
    }

    /* Rehash all existing entries */
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->keys[i] != 0) {
            uint64_t h = sh_hash_i64(map->keys[i]) % new_capacity;
            while (new_keys[h] != 0) {
                h = (h + 1) % new_capacity;
            }
            new_keys[h] = map->keys[i];
            new_values[h] = map->values[i];
        }
    }

    free(map->keys);
    free(map->values);
    map->keys = new_keys;
    map->values = new_values;
    map->capacity = new_capacity;

    return SH_HASHMAP_OK;
}

SHHashmapStatus sh_hashmap_i64_insert(SHHashmapI64 *map, int64_t key, size_t value)
{
    if (!map) return SH_HASHMAP_ERROR_NULL_PARAM;
    if (key == 0) return SH_HASHMAP_ERROR_INVALID_KEY;

    /* Resize if needed */
    if (map->count >= (size_t)((float)map->capacity * map->load_factor)) {
        SHHashmapStatus status = sh_hashmap_i64_resize(map);
        if (status != SH_HASHMAP_OK) return status;
    }

    uint64_t h = sh_hash_i64(key) % map->capacity;
    size_t start = h;

    while (map->keys[h] != 0) {
        if (map->keys[h] == key) {
            /* Update existing entry */
            map->values[h] = value;
            return SH_HASHMAP_OK;
        }
        h = (h + 1) % map->capacity;
        if (h == start) {
            /* Table is full (shouldn't happen with proper load factor) */
            return SH_HASHMAP_ERROR_FULL;
        }
    }

    /* Insert new entry */
    map->keys[h] = key;
    map->values[h] = value;
    map->count++;

    return SH_HASHMAP_OK;
}

size_t sh_hashmap_i64_lookup(const SHHashmapI64 *map, int64_t key)
{
    if (!map || key == 0 || map->capacity == 0) return SIZE_MAX;

    uint64_t h = sh_hash_i64(key) % map->capacity;
    size_t start = h;

    while (map->keys[h] != 0) {
        if (map->keys[h] == key) {
            return map->values[h];
        }
        h = (h + 1) % map->capacity;
        if (h == start) break;
    }

    return SIZE_MAX;
}

int sh_hashmap_i64_contains(const SHHashmapI64 *map, int64_t key)
{
    return sh_hashmap_i64_lookup(map, key) != SIZE_MAX;
}

size_t sh_hashmap_i64_count(const SHHashmapI64 *map)
{
    return map ? map->count : 0;
}

size_t sh_hashmap_i64_capacity(const SHHashmapI64 *map)
{
    return map ? map->capacity : 0;
}

/* ============================================================================
 * SHHashmapI64U32 Implementation (int64 -> uint32_t)
 * ============================================================================ */

SHHashmapI64U32 *sh_hashmap_i64u32_create(size_t expected_size)
{
    return sh_hashmap_i64u32_create_ex(expected_size, SH_HASHMAP_DEFAULT_LOAD_FACTOR);
}

SHHashmapI64U32 *sh_hashmap_i64u32_create_ex(size_t expected_size, float load_factor)
{
    if (load_factor < 0.1f) load_factor = 0.1f;
    if (load_factor > 0.95f) load_factor = 0.95f;

    SHHashmapI64U32 *map = malloc(sizeof(SHHashmapI64U32));
    if (!map) return NULL;

    size_t min_capacity = (size_t)((float)expected_size / load_factor) + 1;
    size_t capacity = next_power_of_two(min_capacity);

    map->keys = calloc(capacity, sizeof(int64_t));
    map->values = calloc(capacity, sizeof(uint32_t));  /* Use calloc for overflow protection */
    if (!map->keys || !map->values) {
        free(map->keys);
        free(map->values);
        free(map);
        return NULL;
    }

    map->capacity = capacity;
    map->count = 0;
    map->load_factor = load_factor;

    return map;
}

void sh_hashmap_i64u32_free(SHHashmapI64U32 *map)
{
    if (!map) return;
    free(map->keys);
    free(map->values);
    free(map);
}

void sh_hashmap_i64u32_clear(SHHashmapI64U32 *map)
{
    if (!map) return;
    memset(map->keys, 0, map->capacity * sizeof(int64_t));
    map->count = 0;
}

static SHHashmapStatus sh_hashmap_i64u32_resize(SHHashmapI64U32 *map)
{
    size_t new_capacity = map->capacity * 2;
    if (new_capacity < map->capacity) {
        return SH_HASHMAP_ERROR_OUT_OF_MEMORY;
    }

    int64_t *new_keys = calloc(new_capacity, sizeof(int64_t));
    uint32_t *new_values = calloc(new_capacity, sizeof(uint32_t));  /* Use calloc for overflow protection */
    if (!new_keys || !new_values) {
        free(new_keys);
        free(new_values);
        return SH_HASHMAP_ERROR_OUT_OF_MEMORY;
    }

    for (size_t i = 0; i < map->capacity; i++) {
        if (map->keys[i] != 0) {
            uint64_t h = sh_hash_i64(map->keys[i]) % new_capacity;
            while (new_keys[h] != 0) {
                h = (h + 1) % new_capacity;
            }
            new_keys[h] = map->keys[i];
            new_values[h] = map->values[i];
        }
    }

    free(map->keys);
    free(map->values);
    map->keys = new_keys;
    map->values = new_values;
    map->capacity = new_capacity;

    return SH_HASHMAP_OK;
}

SHHashmapStatus sh_hashmap_i64u32_insert(SHHashmapI64U32 *map, int64_t key, uint32_t value)
{
    if (!map) return SH_HASHMAP_ERROR_NULL_PARAM;
    if (key == 0) return SH_HASHMAP_ERROR_INVALID_KEY;

    if (map->count >= (size_t)((float)map->capacity * map->load_factor)) {
        SHHashmapStatus status = sh_hashmap_i64u32_resize(map);
        if (status != SH_HASHMAP_OK) return status;
    }

    uint64_t h = sh_hash_i64(key) % map->capacity;
    size_t start = h;

    while (map->keys[h] != 0) {
        if (map->keys[h] == key) {
            map->values[h] = value;
            return SH_HASHMAP_OK;
        }
        h = (h + 1) % map->capacity;
        if (h == start) {
            return SH_HASHMAP_ERROR_FULL;
        }
    }

    map->keys[h] = key;
    map->values[h] = value;
    map->count++;

    return SH_HASHMAP_OK;
}

uint32_t sh_hashmap_i64u32_lookup(const SHHashmapI64U32 *map, int64_t key)
{
    if (!map || key == 0 || map->capacity == 0) return SH_HASHMAP_INVALID_U32;

    uint64_t h = sh_hash_i64(key) % map->capacity;
    size_t start = h;

    while (map->keys[h] != 0) {
        if (map->keys[h] == key) {
            return map->values[h];
        }
        h = (h + 1) % map->capacity;
        if (h == start) break;
    }

    return SH_HASHMAP_INVALID_U32;
}

int sh_hashmap_i64u32_contains(const SHHashmapI64U32 *map, int64_t key)
{
    return sh_hashmap_i64u32_lookup(map, key) != SH_HASHMAP_INVALID_U32;
}

size_t sh_hashmap_i64u32_count(const SHHashmapI64U32 *map)
{
    return map ? map->count : 0;
}

size_t sh_hashmap_i64u32_capacity(const SHHashmapI64U32 *map)
{
    return map ? map->capacity : 0;
}

/* ============================================================================
 * Iterator Implementation
 * ============================================================================ */

void sh_hashmap_i64_iter_init(SHHashmapI64Iter *iter, const SHHashmapI64 *map)
{
    if (!iter) return;
    iter->map = map;
    iter->index = 0;
}

int sh_hashmap_i64_iter_next(SHHashmapI64Iter *iter, int64_t *key, size_t *value)
{
    if (!iter || !iter->map) return 0;

    while (iter->index < iter->map->capacity) {
        if (iter->map->keys[iter->index] != 0) {
            if (key) *key = iter->map->keys[iter->index];
            if (value) *value = iter->map->values[iter->index];
            iter->index++;
            return 1;
        }
        iter->index++;
    }

    return 0;
}

void sh_hashmap_i64u32_iter_init(SHHashmapI64U32Iter *iter, const SHHashmapI64U32 *map)
{
    if (!iter) return;
    iter->map = map;
    iter->index = 0;
}

int sh_hashmap_i64u32_iter_next(SHHashmapI64U32Iter *iter, int64_t *key, uint32_t *value)
{
    if (!iter || !iter->map) return 0;

    while (iter->index < iter->map->capacity) {
        if (iter->map->keys[iter->index] != 0) {
            if (key) *key = iter->map->keys[iter->index];
            if (value) *value = iter->map->values[iter->index];
            iter->index++;
            return 1;
        }
        iter->index++;
    }

    return 0;
}
