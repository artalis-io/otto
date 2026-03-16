#include "ar_operators.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t id;
    double score;
} ARScoredItem;

static int ar_compare_scored_desc(const void *a, const void *b) {
    const ARScoredItem *ia = (const ARScoredItem *)a;
    const ARScoredItem *ib = (const ARScoredItem *)b;

    if (ia->score > ib->score) {
        return -1;
    }
    if (ia->score < ib->score) {
        return 1;
    }
    if (ia->id < ib->id) {
        return -1;
    }
    if (ia->id > ib->id) {
        return 1;
    }
    return 0;
}

static int ar_pick_rank_index(SHRng *rng, int size, double randomness) {
    double exponent = randomness >= 1.0 ? randomness : 1.0;
    double u;
    int idx;

    if (size <= 1) {
        return 0;
    }

    u = sh_rng_uniform(rng);
    idx = (int)(pow(u, exponent) * (double)size);
    if (idx < 0) {
        idx = 0;
    } else if (idx >= size) {
        idx = size - 1;
    }
    return idx;
}

static ARStatus ar_collect_elements(void *solution, ARGetCountFn get_count,
                                    ARGetElementFn get_element, void *user_ctx,
                                    int *count_out, uint32_t **ids_out) {
    int count;
    uint32_t *ids;
    int i;

    if (!solution || !get_count || !get_element || !count_out || !ids_out) {
        return AR_STATUS_INVALID_ARG;
    }

    *count_out = 0;
    *ids_out = NULL;

    count = get_count(solution, user_ctx);
    if (count < 0) {
        return AR_STATUS_INVALID_ARG;
    }
    if (count == 0) {
        return AR_STATUS_OK;
    }

    ids = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (!ids) {
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < count; i++) {
        ids[i] = get_element(solution, user_ctx, i);
    }

    *count_out = count;
    *ids_out = ids;
    return AR_STATUS_OK;
}

ARStatus ar_remove_random(SHRng *rng, void *solution, int q, uint32_t *removed,
                          ARGetCountFn get_count, ARGetElementFn get_element,
                          void *user_ctx, int *removed_count) {
    ARStatus status;
    uint32_t *ids = NULL;
    int count = 0;
    int n;
    int i;

    if (!rng || !removed_count || q < 0) {
        return AR_STATUS_INVALID_ARG;
    }
    *removed_count = 0;
    if (q == 0) {
        return AR_STATUS_OK;
    }
    if (!removed || !get_count || !get_element) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_collect_elements(solution, get_count, get_element, user_ctx,
                                 &count, &ids);
    if (status != AR_STATUS_OK) {
        return status;
    }
    if (count == 0) {
        return AR_STATUS_OK;
    }

    n = q < count ? q : count;
    for (i = 0; i < n; i++) {
        int j = sh_rng_int_range(rng, i, count - 1);
        uint32_t tmp = ids[i];
        ids[i] = ids[j];
        ids[j] = tmp;
        removed[i] = ids[i];
    }

    *removed_count = n;
    free(ids);
    return AR_STATUS_OK;
}

ARStatus ar_remove_worst(SHRng *rng, void *ctx, void *solution, int q,
                         uint32_t *removed, ARGetCountFn get_count,
                         ARGetElementFn get_element, ARRemovalCostFn removal_cost,
                         double randomness, void *user_ctx, int *removed_count) {
    ARStatus status;
    ARScoredItem *items = NULL;
    uint32_t *ids = NULL;
    int count = 0;
    int remaining;
    int n;
    int i;

    if (!rng || !removed_count || q < 0) {
        return AR_STATUS_INVALID_ARG;
    }
    *removed_count = 0;
    if (q == 0) {
        return AR_STATUS_OK;
    }
    if (!removed || !get_count || !get_element || !removal_cost) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_collect_elements(solution, get_count, get_element, user_ctx,
                                 &count, &ids);
    if (status != AR_STATUS_OK) {
        return status;
    }
    if (count == 0) {
        return AR_STATUS_OK;
    }

    items = (ARScoredItem *)malloc((size_t)count * sizeof(ARScoredItem));
    if (!items) {
        free(ids);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < count; i++) {
        double cost = removal_cost(ctx, solution, ids[i]);
        items[i].id = ids[i];
        items[i].score = isfinite(cost) ? cost : -INFINITY;
    }
    qsort(items, (size_t)count, sizeof(ARScoredItem), ar_compare_scored_desc);

    remaining = count;
    n = q < count ? q : count;
    for (i = 0; i < n; i++) {
        int idx = ar_pick_rank_index(rng, remaining, randomness);
        removed[i] = items[idx].id;
        if (idx < (remaining - 1)) {
            memmove(&items[idx], &items[idx + 1],
                    (size_t)(remaining - idx - 1) * sizeof(ARScoredItem));
        }
        remaining--;
    }

    *removed_count = n;
    free(items);
    free(ids);
    return AR_STATUS_OK;
}

ARStatus ar_remove_related(SHRng *rng, void *ctx, void *solution, int q,
                           uint32_t *removed, ARGetCountFn get_count,
                           ARGetElementFn get_element,
                           ARRelatednessFn relatedness, double randomness,
                           void *user_ctx, int *removed_count) {
    ARStatus status;
    uint32_t *ids = NULL;
    ARScoredItem *items = NULL;
    int count = 0;
    int n;
    int remaining;
    int i;
    int seed_idx;
    uint32_t seed;

    if (!rng || !removed_count || q < 0) {
        return AR_STATUS_INVALID_ARG;
    }
    *removed_count = 0;
    if (q == 0) {
        return AR_STATUS_OK;
    }
    if (!removed || !get_count || !get_element || !relatedness) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_collect_elements(solution, get_count, get_element, user_ctx,
                                 &count, &ids);
    if (status != AR_STATUS_OK) {
        return status;
    }
    if (count == 0) {
        return AR_STATUS_OK;
    }

    n = q < count ? q : count;
    seed_idx = sh_rng_int_range(rng, 0, count - 1);
    seed = ids[seed_idx];
    removed[0] = seed;
    *removed_count = 1;

    if (n == 1) {
        free(ids);
        return AR_STATUS_OK;
    }

    ids[seed_idx] = ids[count - 1];
    count--;

    items = (ARScoredItem *)malloc((size_t)count * sizeof(ARScoredItem));
    if (!items) {
        free(ids);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < count; i++) {
        double rel = relatedness(ctx, seed, ids[i]);
        items[i].id = ids[i];
        items[i].score = isfinite(rel) ? rel : -INFINITY;
    }
    qsort(items, (size_t)count, sizeof(ARScoredItem), ar_compare_scored_desc);

    remaining = count;
    for (i = 1; i < n; i++) {
        int idx = ar_pick_rank_index(rng, remaining, randomness);
        removed[i] = items[idx].id;
        if (idx < (remaining - 1)) {
            memmove(&items[idx], &items[idx + 1],
                    (size_t)(remaining - idx - 1) * sizeof(ARScoredItem));
        }
        remaining--;
        (*removed_count)++;
    }

    free(items);
    free(ids);
    return AR_STATUS_OK;
}
