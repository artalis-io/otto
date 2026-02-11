#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "arbor.h"

typedef struct {
    uint32_t ids[16];
    int count;
} TestList;

static int list_count(void *solution, void *user_ctx) {
    TestList *list = (TestList *)solution;
    (void)user_ctx;
    return list->count;
}

static uint32_t list_element(void *solution, void *user_ctx, int index) {
    TestList *list = (TestList *)solution;
    (void)user_ctx;
    if (index < 0 || index >= list->count) {
        return UINT32_MAX;
    }
    return list->ids[index];
}

static double removal_cost(void *ctx, void *solution, uint32_t element_id) {
    (void)ctx;
    (void)solution;
    return (double)element_id;
}

static double relatedness(void *ctx, uint32_t a, uint32_t b) {
    int delta = (int)a - (int)b;
    (void)ctx;
    if (delta < 0) {
        delta = -delta;
    }
    return -(double)delta;
}

static int contains(const TestList *list, uint32_t id) {
    int i;
    for (i = 0; i < list->count; i++) {
        if (list->ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

static void assert_unique_subset(const TestList *list,
                                 const uint32_t *removed,
                                 int removed_count) {
    int i;
    int j;
    for (i = 0; i < removed_count; i++) {
        assert(contains(list, removed[i]));
        for (j = i + 1; j < removed_count; j++) {
            assert(removed[i] != removed[j]);
        }
    }
}

static uint32_t nearest_neighbor(const TestList *list, uint32_t seed) {
    int i;
    double best = -1e300;
    uint32_t best_id = UINT32_MAX;

    for (i = 0; i < list->count; i++) {
        uint32_t id = list->ids[i];
        double score;
        if (id == seed) {
            continue;
        }
        score = relatedness(NULL, seed, id);
        if (score > best) {
            best = score;
            best_id = id;
        }
    }
    return best_id;
}

int main(void) {
    SHRng *rng = sh_rng_create_default();
    uint32_t removed_a[8];
    uint32_t removed_b[8];
    int removed_count_a = 0;
    int removed_count_b = 0;
    ARStatus status;
    int i;

    TestList random_list = { .ids = { 1, 2, 3, 4, 5 }, .count = 5 };
    TestList worst_list = { .ids = { 1, 2, 3, 4, 5 }, .count = 5 };
    TestList related_list = { .ids = { 1, 4, 9, 16 }, .count = 4 };

    assert(rng != NULL);

    sh_rng_seed(rng, 1337);
    status = ar_remove_random(rng, &random_list, 3, removed_a,
                              list_count, list_element, NULL, &removed_count_a);
    assert(status == AR_STATUS_OK);
    assert(removed_count_a == 3);
    assert_unique_subset(&random_list, removed_a, removed_count_a);

    sh_rng_seed(rng, 1337);
    status = ar_remove_random(rng, &random_list, 3, removed_b,
                              list_count, list_element, NULL, &removed_count_b);
    assert(status == AR_STATUS_OK);
    assert(removed_count_b == removed_count_a);
    for (i = 0; i < removed_count_a; i++) {
        assert(removed_a[i] == removed_b[i]);
    }

    sh_rng_seed(rng, 2026);
    status = ar_remove_worst(rng, NULL, &worst_list, 2, removed_a,
                             list_count, list_element, removal_cost,
                             1000000.0, NULL, &removed_count_a);
    assert(status == AR_STATUS_OK);
    assert(removed_count_a == 2);
    assert(removed_a[0] == 5);
    assert(removed_a[1] == 4);

    sh_rng_seed(rng, 77);
    status = ar_remove_related(rng, NULL, &related_list, 2, removed_a,
                               list_count, list_element, relatedness,
                               1000000.0, NULL, &removed_count_a);
    assert(status == AR_STATUS_OK);
    assert(removed_count_a == 2);
    assert_unique_subset(&related_list, removed_a, removed_count_a);
    assert(removed_a[1] == nearest_neighbor(&related_list, removed_a[0]));

    sh_rng_free(rng);
    printf("arbor operators test passed\n");
    return 0;
}
