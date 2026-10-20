#ifndef SURGE_SG_API_COUNTS_H
#define SURGE_SG_API_COUNTS_H

#include <stddef.h>
#include <stdint.h>

/* JSON arrays use size_t, but the solver consumes uint32_t counts. Check both
 * that boundary and allocation arithmetic before narrowing or allocating.
 * On failure the caller-owned accumulator is unchanged. */
static inline int sg_api_count_add(size_t *total, size_t count, size_t item_size) {
    size_t sum;
    if (!total || item_size == 0 || count > UINT32_MAX ||
        *total > UINT32_MAX - count) {
        return -1;
    }
    sum = *total + count;
    if (sum > SIZE_MAX / item_size) return -1;
    *total = sum;
    return 0;
}

#endif /* SURGE_SG_API_COUNTS_H */
