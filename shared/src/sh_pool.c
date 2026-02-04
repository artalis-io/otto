/*
 * sh_pool.c - Contiguous memory pool implementation
 */

#include "sh_pool.h"
#include <stdlib.h>
#include <string.h>

int sh_pool_init(SHPool *pool, size_t elem_size, size_t capacity)
{
    if (!pool || elem_size == 0) return -1;

    pool->data = malloc(elem_size * capacity);
    if (!pool->data && capacity > 0) return -1;

    pool->elem_size = elem_size;
    pool->capacity = capacity;
    pool->used = 0;
    return 0;
}

size_t sh_pool_alloc(SHPool *pool, size_t count)
{
    if (!pool || !pool->data) return SH_POOL_INVALID;
    if (pool->used + count > pool->capacity) return SH_POOL_INVALID;

    size_t offset = pool->used;
    pool->used += count;
    return offset;
}

void *sh_pool_ptr(const SHPool *pool, size_t offset)
{
    if (!pool || !pool->data) return NULL;
    if (offset >= pool->used && offset != 0) return NULL;
    /* Allow offset 0 even for empty pool (for allocation start) */
    if (offset > pool->capacity) return NULL;

    return (char*)pool->data + offset * pool->elem_size;
}

size_t sh_pool_remaining(const SHPool *pool)
{
    if (!pool) return 0;
    return pool->capacity - pool->used;
}

size_t sh_pool_used(const SHPool *pool)
{
    if (!pool) return 0;
    return pool->used;
}

void sh_pool_reset(SHPool *pool)
{
    if (pool) {
        pool->used = 0;
    }
}

void sh_pool_free(SHPool *pool)
{
    if (pool) {
        free(pool->data);
        pool->data = NULL;
        pool->capacity = 0;
        pool->used = 0;
    }
}

int sh_pool_grow(SHPool *pool, double factor)
{
    if (!pool || factor <= 1.0) return -1;

    size_t new_capacity = (size_t)(pool->capacity * factor);
    if (new_capacity <= pool->capacity) {
        new_capacity = pool->capacity + 1;  /* At least grow by 1 */
    }

    return sh_pool_ensure_capacity(pool, new_capacity);
}

int sh_pool_ensure_capacity(SHPool *pool, size_t min_capacity)
{
    if (!pool) return -1;
    if (pool->capacity >= min_capacity) return 0;

    void *new_data = realloc(pool->data, pool->elem_size * min_capacity);
    if (!new_data) return -1;

    pool->data = new_data;
    pool->capacity = min_capacity;
    return 0;
}
