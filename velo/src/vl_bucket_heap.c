/*
 * vl_bucket_heap.c - Bucket-based priority queue
 *
 * NOTE: This implementation does NOT provide a speedup for road routing with
 * floating-point distances. The bucket heap is designed for integer edge weights
 * with bounded range (like Dial's algorithm). For floating-point distances,
 * finding the true minimum within a bucket requires O(bucket_size) scanning,
 * which negates the O(1) amortized benefit.
 *
 * For road routing, use the binary heap (vl_heap.c) instead.
 *
 * This code is kept for reference and potential future use with integer weights.
 */

#include "vl_types.h"
#include <stdlib.h>
#include <string.h>

/* Bucket width in kilometers (same units as our distances) */
#define BUCKET_WIDTH_KM 0.1  /* 100 meters */

/* Maximum distance in km (Hungary is ~500km across, use 2000km for safety) */
#define MAX_DISTANCE_KM 2000.0

/* Number of buckets */
#define NUM_BUCKETS ((int)(MAX_DISTANCE_KM / BUCKET_WIDTH_KM) + 1)

/* ============================================================================
 * Bucket Heap Structure
 * ============================================================================ */

typedef struct VLBucketNode {
    uint32_t node;
    double priority;
    struct VLBucketNode *next;
} VLBucketNode;

typedef struct VLBucketHeap {
    VLBucketNode **buckets;  /* Array of bucket heads */
    int num_buckets;
    int current_bucket;      /* Current minimum bucket index */
    size_t size;             /* Total number of elements */

    /* Node -> priority mapping for decrease-key */
    double *priorities;
    int *in_heap;            /* 1 if node is in heap */
    uint32_t num_nodes;

    /* Free list for node reuse */
    VLBucketNode *free_list;
} VLBucketHeap;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static inline int priority_to_bucket(double priority)
{
    int bucket = (int)(priority / BUCKET_WIDTH_KM);
    if (bucket < 0) bucket = 0;
    if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
    return bucket;
}

static VLBucketNode *alloc_node(VLBucketHeap *heap)
{
    if (heap->free_list) {
        VLBucketNode *node = heap->free_list;
        heap->free_list = node->next;
        return node;
    }
    return malloc(sizeof(VLBucketNode));
}

static void free_node(VLBucketHeap *heap, VLBucketNode *node)
{
    node->next = heap->free_list;
    heap->free_list = node;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

VLBucketHeap *vl_bucket_heap_create(uint32_t num_nodes)
{
    VLBucketHeap *heap = calloc(1, sizeof(VLBucketHeap));
    if (!heap) return NULL;

    heap->num_buckets = NUM_BUCKETS;
    heap->buckets = calloc(NUM_BUCKETS, sizeof(VLBucketNode *));
    if (!heap->buckets) {
        free(heap);
        return NULL;
    }

    heap->priorities = malloc(num_nodes * sizeof(double));
    heap->in_heap = calloc(num_nodes, sizeof(int));
    if (!heap->priorities || !heap->in_heap) {
        free(heap->buckets);
        free(heap->priorities);
        free(heap->in_heap);
        free(heap);
        return NULL;
    }

    for (uint32_t i = 0; i < num_nodes; i++) {
        heap->priorities[i] = VL_INF;
    }

    heap->num_nodes = num_nodes;
    heap->current_bucket = 0;
    heap->size = 0;
    heap->free_list = NULL;

    return heap;
}

void vl_bucket_heap_free(VLBucketHeap *heap)
{
    if (!heap) return;

    /* Free all nodes in buckets */
    for (int i = 0; i < heap->num_buckets; i++) {
        VLBucketNode *node = heap->buckets[i];
        while (node) {
            VLBucketNode *next = node->next;
            free(node);
            node = next;
        }
    }

    /* Free free list */
    VLBucketNode *node = heap->free_list;
    while (node) {
        VLBucketNode *next = node->next;
        free(node);
        node = next;
    }

    free(heap->buckets);
    free(heap->priorities);
    free(heap->in_heap);
    free(heap);
}

void vl_bucket_heap_clear(VLBucketHeap *heap)
{
    if (!heap) return;

    /* Move all nodes to free list */
    for (int i = 0; i < heap->num_buckets; i++) {
        VLBucketNode *node = heap->buckets[i];
        while (node) {
            VLBucketNode *next = node->next;
            heap->in_heap[node->node] = 0;
            free_node(heap, node);
            node = next;
        }
        heap->buckets[i] = NULL;
    }

    heap->current_bucket = 0;
    heap->size = 0;
}

int vl_bucket_heap_empty(const VLBucketHeap *heap)
{
    return heap->size == 0;
}

size_t vl_bucket_heap_size(const VLBucketHeap *heap)
{
    return heap->size;
}

VLStatus vl_bucket_heap_push(VLBucketHeap *heap, uint32_t node, double priority)
{
    if (!heap || node >= heap->num_nodes) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* If already in heap with lower priority, skip */
    if (heap->in_heap[node]) {
        if (priority >= heap->priorities[node]) {
            return VL_OK;
        }
        /* Need to update - mark old entry as stale (lazy deletion) */
        /* We'll skip it when we pop */
    }

    int bucket = priority_to_bucket(priority);

    VLBucketNode *bn = alloc_node(heap);
    if (!bn) return VL_ERROR_OUT_OF_MEMORY;

    bn->node = node;
    bn->priority = priority;
    bn->next = heap->buckets[bucket];
    heap->buckets[bucket] = bn;

    heap->priorities[node] = priority;
    heap->in_heap[node] = 1;
    heap->size++;

    /* Update current bucket if this is smaller */
    if (bucket < heap->current_bucket) {
        heap->current_bucket = bucket;
    }

    return VL_OK;
}

VLStatus vl_bucket_heap_pop(VLBucketHeap *heap, VLHeapEntry *entry)
{
    if (!heap || heap->size == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Find first non-empty bucket with valid entries */
    while (heap->current_bucket < heap->num_buckets) {
        /* Clean stale entries and find true minimum in this bucket */
        VLBucketNode **prev = &heap->buckets[heap->current_bucket];
        VLBucketNode *node = *prev;
        VLBucketNode **min_prev = NULL;
        VLBucketNode *min_node = NULL;
        double min_priority = VL_INF;

        while (node) {
            /* Check if this entry is still valid (not stale from decrease-key) */
            if (heap->in_heap[node->node] &&
                node->priority <= heap->priorities[node->node] + 1e-9) {
                /* Valid entry - check if it's the minimum */
                if (node->priority < min_priority) {
                    min_priority = node->priority;
                    min_node = node;
                    min_prev = prev;
                }
                prev = &node->next;
                node = node->next;
            } else {
                /* Stale entry - remove it */
                *prev = node->next;
                VLBucketNode *to_free = node;
                node = node->next;
                free_node(heap, to_free);
            }
        }

        if (min_node) {
            /* Found valid minimum */
            if (entry) {
                entry->node = min_node->node;
                entry->priority = min_node->priority;
            }

            heap->in_heap[min_node->node] = 0;
            *min_prev = min_node->next;
            free_node(heap, min_node);
            heap->size--;

            return VL_OK;
        }

        heap->current_bucket++;
    }

    return VL_ERROR_INVALID_ARGUMENT;  /* Should not happen if size > 0 */
}

int vl_bucket_heap_contains(const VLBucketHeap *heap, uint32_t node)
{
    if (node >= heap->num_nodes) return 0;
    return heap->in_heap[node];
}

double vl_bucket_heap_priority(const VLBucketHeap *heap, uint32_t node)
{
    if (node >= heap->num_nodes) return VL_INF;
    if (!heap->in_heap[node]) return VL_INF;
    return heap->priorities[node];
}
