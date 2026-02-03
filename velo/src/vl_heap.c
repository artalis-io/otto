/*
 * vl_heap.c - 4-ary min-heap priority queue
 *
 * Efficient priority queue for Dijkstra and A* algorithms.
 * Supports push, pop, and decrease-key operations.
 *
 * Uses a 4-ary heap (each node has 4 children) instead of binary heap.
 * This reduces tree height from log2(n) to log4(n) and improves cache
 * locality since 4 children often fit in a single cache line.
 */

#include "vl_types.h"
#include <stdlib.h>
#include <string.h>

/* Forward declaration */
VLStatus vl_heap_decrease_key(VLHeap *heap, uint32_t node, double new_priority);

/* ============================================================================
 * Internal Helpers (4-ary heap)
 * ============================================================================ */

/* 4-ary heap: parent at (i-1)/4, children at 4*i+1, 4*i+2, 4*i+3, 4*i+4 */
static inline size_t parent(size_t i) { return (i - 1) / 4; }
static inline size_t first_child(size_t i) { return 4 * i + 1; }

static void swap_entries(VLHeap *heap, size_t i, size_t j)
{
    VLHeapEntry tmp = heap->entries[i];
    heap->entries[i] = heap->entries[j];
    heap->entries[j] = tmp;

    /* Update position tracking */
    heap->positions[heap->entries[i].node] = (uint32_t)i;
    heap->positions[heap->entries[j].node] = (uint32_t)j;
}

static void sift_up(VLHeap *heap, size_t i)
{
    while (i > 0) {
        size_t p = parent(i);
        if (heap->entries[p].priority <= heap->entries[i].priority) {
            break;
        }
        swap_entries(heap, i, p);
        i = p;
    }
}

static void sift_down(VLHeap *heap, size_t i)
{
    while (1) {
        size_t fc = first_child(i);
        if (fc >= heap->size) break;

        /* Find minimum among up to 4 children */
        size_t smallest = i;
        double smallest_pri = heap->entries[i].priority;

        /* Check all 4 children (unrolled for performance) */
        if (fc < heap->size && heap->entries[fc].priority < smallest_pri) {
            smallest = fc;
            smallest_pri = heap->entries[fc].priority;
        }
        if (fc + 1 < heap->size && heap->entries[fc + 1].priority < smallest_pri) {
            smallest = fc + 1;
            smallest_pri = heap->entries[fc + 1].priority;
        }
        if (fc + 2 < heap->size && heap->entries[fc + 2].priority < smallest_pri) {
            smallest = fc + 2;
            smallest_pri = heap->entries[fc + 2].priority;
        }
        if (fc + 3 < heap->size && heap->entries[fc + 3].priority < smallest_pri) {
            smallest = fc + 3;
        }

        if (smallest == i) break;

        swap_entries(heap, i, smallest);
        i = smallest;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/*
 * Create a new min-heap.
 * num_nodes is the total number of nodes in the graph (for position tracking).
 */
VLHeap *vl_heap_create(size_t num_nodes)
{
    VLHeap *heap = malloc(sizeof(VLHeap));
    if (!heap) return NULL;

    heap->capacity = (num_nodes < 1024) ? 1024 : num_nodes / 4;
    heap->entries = malloc(heap->capacity * sizeof(VLHeapEntry));
    if (!heap->entries) {
        free(heap);
        return NULL;
    }

    heap->positions = malloc(num_nodes * sizeof(uint32_t));
    if (!heap->positions) {
        free(heap->entries);
        free(heap);
        return NULL;
    }

    /* Initialize all positions to invalid */
    for (size_t i = 0; i < num_nodes; i++) {
        heap->positions[i] = UINT32_MAX;
    }

    heap->size = 0;
    heap->num_nodes = num_nodes;

    return heap;
}

/*
 * Free heap memory.
 */
void vl_heap_free(VLHeap *heap)
{
    if (!heap) return;
    free(heap->entries);
    free(heap->positions);
    free(heap);
}

/*
 * Clear the heap for reuse.
 */
void vl_heap_clear(VLHeap *heap)
{
    if (!heap) return;

    /* Reset positions for nodes that were in the heap */
    for (size_t i = 0; i < heap->size; i++) {
        heap->positions[heap->entries[i].node] = UINT32_MAX;
    }

    heap->size = 0;
}

/*
 * Check if heap is empty.
 */
int vl_heap_empty(const VLHeap *heap)
{
    return heap->size == 0;
}

/*
 * Get current heap size.
 */
size_t vl_heap_size(const VLHeap *heap)
{
    return heap->size;
}

/*
 * Check if a node is in the heap.
 */
int vl_heap_contains(const VLHeap *heap, uint32_t node)
{
    if (node >= heap->num_nodes) return 0;
    return heap->positions[node] != UINT32_MAX;
}

/*
 * Get priority of a node in the heap.
 * Returns VL_INF if node is not in heap.
 */
double vl_heap_priority(const VLHeap *heap, uint32_t node)
{
    if (node >= heap->num_nodes) return VL_INF;
    uint32_t pos = heap->positions[node];
    if (pos == UINT32_MAX) return VL_INF;
    return heap->entries[pos].priority;
}

/*
 * Push a node with given priority.
 * If node is already in heap, updates priority if new value is smaller.
 * Returns VL_OK on success.
 */
VLStatus vl_heap_push(VLHeap *heap, uint32_t node, double priority)
{
    if (!heap || node >= heap->num_nodes) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Check if already in heap */
    if (heap->positions[node] != UINT32_MAX) {
        return vl_heap_decrease_key(heap, node, priority);
    }

    /* Grow capacity if needed */
    if (heap->size >= heap->capacity) {
        size_t new_cap = heap->capacity * 2;
        VLHeapEntry *new_entries = realloc(heap->entries,
                                           new_cap * sizeof(VLHeapEntry));
        if (!new_entries) {
            return VL_ERROR_OUT_OF_MEMORY;
        }
        heap->entries = new_entries;
        heap->capacity = new_cap;
    }

    /* Add at end and sift up */
    size_t i = heap->size;
    heap->entries[i].node = node;
    heap->entries[i].priority = priority;
    heap->positions[node] = (uint32_t)i;
    heap->size++;

    sift_up(heap, i);

    return VL_OK;
}

/*
 * Pop the minimum element from the heap.
 * Returns VL_OK on success, stores result in *entry.
 */
VLStatus vl_heap_pop(VLHeap *heap, VLHeapEntry *entry)
{
    if (!heap || heap->size == 0) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    /* Copy minimum element */
    if (entry) {
        *entry = heap->entries[0];
    }

    /* Mark node as no longer in heap */
    heap->positions[heap->entries[0].node] = UINT32_MAX;

    /* Move last element to root and sift down */
    heap->size--;
    if (heap->size > 0) {
        heap->entries[0] = heap->entries[heap->size];
        heap->positions[heap->entries[0].node] = 0;
        sift_down(heap, 0);
    }

    return VL_OK;
}

/*
 * Peek at the minimum element without removing it.
 */
VLStatus vl_heap_peek(const VLHeap *heap, VLHeapEntry *entry)
{
    if (!heap || heap->size == 0 || !entry) {
        return VL_ERROR_INVALID_ARGUMENT;
    }
    *entry = heap->entries[0];
    return VL_OK;
}

/*
 * Decrease the priority of a node in the heap.
 * No effect if new priority >= current priority.
 * Returns VL_OK on success.
 */
VLStatus vl_heap_decrease_key(VLHeap *heap, uint32_t node, double new_priority)
{
    if (!heap || node >= heap->num_nodes) {
        return VL_ERROR_INVALID_ARGUMENT;
    }

    uint32_t pos = heap->positions[node];
    if (pos == UINT32_MAX) {
        /* Node not in heap, push it instead */
        return vl_heap_push(heap, node, new_priority);
    }

    /* Only decrease, never increase */
    if (new_priority >= heap->entries[pos].priority) {
        return VL_OK;
    }

    heap->entries[pos].priority = new_priority;
    sift_up(heap, pos);

    return VL_OK;
}

/*
 * Push or decrease key (convenient combined operation).
 */
VLStatus vl_heap_push_or_decrease(VLHeap *heap, uint32_t node, double priority)
{
    if (vl_heap_contains(heap, node)) {
        return vl_heap_decrease_key(heap, node, priority);
    } else {
        return vl_heap_push(heap, node, priority);
    }
}
