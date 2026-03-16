#ifndef ARBOR_AR_OPERATORS_H
#define ARBOR_AR_OPERATORS_H

#include <stdint.h>

#include "ar_types.h"
#include "sh_dist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ARGetCountFn)(void *solution, void *user_ctx);
typedef uint32_t (*ARGetElementFn)(void *solution, void *user_ctx, int index);
typedef double (*ARRemovalCostFn)(void *ctx, void *solution, uint32_t element_id);
typedef double (*ARRelatednessFn)(void *ctx, uint32_t a, uint32_t b);

ARStatus ar_remove_random(SHRng *rng, void *solution, int q, uint32_t *removed,
                          ARGetCountFn get_count, ARGetElementFn get_element,
                          void *user_ctx, int *removed_count);

ARStatus ar_remove_worst(SHRng *rng, void *ctx, void *solution, int q,
                         uint32_t *removed, ARGetCountFn get_count,
                         ARGetElementFn get_element, ARRemovalCostFn removal_cost,
                         double randomness, void *user_ctx, int *removed_count);

ARStatus ar_remove_related(SHRng *rng, void *ctx, void *solution, int q,
                           uint32_t *removed, ARGetCountFn get_count,
                           ARGetElementFn get_element,
                           ARRelatednessFn relatedness, double randomness,
                           void *user_ctx, int *removed_count);

#ifdef __cplusplus
}
#endif

#endif /* ARBOR_AR_OPERATORS_H */
