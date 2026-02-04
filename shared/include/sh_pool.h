/*
 * sh_pool.h - Contiguous memory pool for OTTO Platform
 *
 * Provides a contiguous buffer with offset tracking for efficient storage
 * of variable-length arrays. Similar to Ralph's spike pool pattern.
 *
 * Use cases:
 * - Storing many small arrays without per-array malloc overhead
 * - Coordinate storage for OSM ways (offset + count instead of pointer)
 * - Sparse matrix data pools
 *
 * Usage:
 *   SHPool pool;
 *   sh_pool_init(&pool, sizeof(CTCoord), 100000);  // 100K coordinates
 *
 *   size_t offset = sh_pool_alloc(&pool, 50);  // Reserve 50 coords
 *   CTCoord *coords = sh_pool_ptr(&pool, offset);
 *   // ... fill coords ...
 *
 *   // Later: access via offset
 *   CTCoord *way_coords = sh_pool_ptr(&pool, way->coord_offset);
 *
 *   sh_pool_reset(&pool);  // Reuse pool
 *   sh_pool_free(&pool);   // Release memory
 */

#ifndef SH_POOL_H
#define SH_POOL_H

#include <stddef.h>
#include <stdint.h>

/* Invalid offset sentinel */
#define SH_POOL_INVALID ((size_t)-1)

/* Contiguous memory pool */
typedef struct SHPool {
    void *data;         /* Contiguous buffer */
    size_t elem_size;   /* Size of each element */
    size_t capacity;    /* Number of elements that fit */
    size_t used;        /* Number of elements used */
} SHPool;

/*
 * Initialize a pool with given element size and capacity.
 * Returns 0 on success, -1 on allocation failure.
 */
int sh_pool_init(SHPool *pool, size_t elem_size, size_t capacity);

/*
 * Allocate count elements from pool.
 * Returns offset (index) of first element, or SH_POOL_INVALID if full.
 */
size_t sh_pool_alloc(SHPool *pool, size_t count);

/*
 * Get pointer to element at given offset.
 * Returns NULL if pool is NULL or offset is out of bounds.
 */
void *sh_pool_ptr(const SHPool *pool, size_t offset);

/*
 * Get remaining capacity in pool (number of elements).
 */
size_t sh_pool_remaining(const SHPool *pool);

/*
 * Get number of elements used in pool.
 */
size_t sh_pool_used(const SHPool *pool);

/*
 * Reset pool for reuse (does not free memory).
 */
void sh_pool_reset(SHPool *pool);

/*
 * Free pool memory and reset to empty state.
 */
void sh_pool_free(SHPool *pool);

/*
 * Grow pool capacity by given factor (e.g., 2.0 = double).
 * Returns 0 on success, -1 on allocation failure.
 */
int sh_pool_grow(SHPool *pool, double factor);

/*
 * Ensure pool has space for at least min_capacity elements.
 * Returns 0 on success, -1 on allocation failure.
 */
int sh_pool_ensure_capacity(SHPool *pool, size_t min_capacity);

/* ============================================================================
 * Typed Pool Macros
 *
 * Convenience macros for type-safe pool access.
 * ============================================================================ */

#define SH_POOL_INIT(pool, type, cap) \
    sh_pool_init((pool), sizeof(type), (cap))

#define SH_POOL_PTR(pool, type, offset) \
    ((type*)sh_pool_ptr((pool), (offset)))

#define SH_POOL_AT(pool, type, offset, idx) \
    (SH_POOL_PTR(pool, type, offset)[(idx)])

#endif /* SH_POOL_H */
