/*
 * sh_heap.c - 4-ary min-heap with position tracking
 *
 * 4-ary heap properties:
 * - Parent of node i: (i - 1) / 4
 * - Children of node i: 4*i + 1, 4*i + 2, 4*i + 3, 4*i + 4
 * - Height: log4(n) vs log2(n) for binary heap
 * - Better cache locality: 4 children fit in one cache line
 */

#include "sh_heap.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Internal Structure
 * ============================================================================ */

struct SHHeap {
    SHHeapEntry *entries;     /* Array of heap entries */
    uint32_t *positions;      /* positions[node] = index in entries, or INVALID */
    size_t size;              /* Current number of entries */
    size_t capacity;          /* Allocated capacity for entries */
    size_t num_nodes;         /* Size of positions array */
};

/* ============================================================================
 * 4-ary Heap Index Functions
 * ============================================================================ */

static inline size_t parent(size_t i)
{
    return (i - 1) / 4;
}

static inline size_t first_child(size_t i)
{
    return 4 * i + 1;
}

/* ============================================================================
 * Internal Operations
 * ============================================================================ */

static void swap_entries(SHHeap *heap, size_t i, size_t j)
{
    SHHeapEntry tmp = heap->entries[i];
    heap->entries[i] = heap->entries[j];
    heap->entries[j] = tmp;

    /* Update position tracking */
    heap->positions[heap->entries[i].node] = (uint32_t)i;
    heap->positions[heap->entries[j].node] = (uint32_t)j;
}

static void sift_up(SHHeap *heap, size_t i)
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

static void sift_down(SHHeap *heap, size_t i)
{
    while (1) {
        size_t fc = first_child(i);
        if (fc >= heap->size) break;

        /* Find minimum among up to 4 children (unrolled for performance) */
        size_t smallest = i;
        double smallest_pri = heap->entries[i].priority;

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
 * Creation and Destruction
 * ============================================================================ */

SHHeap *sh_heap_create(size_t num_nodes)
{
    SHHeap *heap = malloc(sizeof(SHHeap));
    if (!heap) return NULL;

    /* Initial capacity: at least 256, or num_nodes/4 for reasonable default */
    heap->capacity = (num_nodes < 1024) ? 256 : num_nodes / 4;
    if (heap->capacity < 256) heap->capacity = 256;

    heap->entries = malloc(heap->capacity * sizeof(SHHeapEntry));
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
        heap->positions[i] = SH_HEAP_INVALID_POS;
    }

    heap->size = 0;
    heap->num_nodes = num_nodes;

    return heap;
}

void sh_heap_free(SHHeap *heap)
{
    if (!heap) return;
    free(heap->entries);
    free(heap->positions);
    free(heap);
}

void sh_heap_clear(SHHeap *heap)
{
    if (!heap) return;

    /* Reset positions only for nodes that were in the heap */
    for (size_t i = 0; i < heap->size; i++) {
        heap->positions[heap->entries[i].node] = SH_HEAP_INVALID_POS;
    }

    heap->size = 0;
}

/* ============================================================================
 * Query Operations
 * ============================================================================ */

int sh_heap_empty(const SHHeap *heap)
{
    return !heap || heap->size == 0;
}

size_t sh_heap_size(const SHHeap *heap)
{
    return heap ? heap->size : 0;
}

int sh_heap_contains(const SHHeap *heap, uint32_t node)
{
    if (!heap || node >= heap->num_nodes) return 0;
    return heap->positions[node] != SH_HEAP_INVALID_POS;
}

double sh_heap_priority(const SHHeap *heap, uint32_t node)
{
    if (!heap || node >= heap->num_nodes) return SH_HEAP_INF;
    uint32_t pos = heap->positions[node];
    if (pos == SH_HEAP_INVALID_POS) return SH_HEAP_INF;
    return heap->entries[pos].priority;
}

/* ============================================================================
 * Heap Operations
 * ============================================================================ */

SHHeapStatus sh_heap_push(SHHeap *heap, uint32_t node, double priority)
{
    if (!heap) return SH_HEAP_ERROR_NULL_PARAM;
    if (node >= heap->num_nodes) return SH_HEAP_ERROR_INVALID_NODE;

    /* If already in heap, use decrease_key instead */
    if (heap->positions[node] != SH_HEAP_INVALID_POS) {
        return sh_heap_decrease_key(heap, node, priority);
    }

    /* Grow capacity if needed (with overflow check) */
    if (heap->size >= heap->capacity) {
        size_t new_cap = heap->capacity * 2;
        if (new_cap <= heap->capacity) {
            return SH_HEAP_ERROR_OUT_OF_MEMORY;
        }
        SHHeapEntry *new_entries = realloc(heap->entries, new_cap * sizeof(SHHeapEntry));
        if (!new_entries) {
            return SH_HEAP_ERROR_OUT_OF_MEMORY;
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

    return SH_HEAP_OK;
}

SHHeapStatus sh_heap_pop(SHHeap *heap, SHHeapEntry *entry)
{
    if (!heap) return SH_HEAP_ERROR_NULL_PARAM;
    if (heap->size == 0) return SH_HEAP_ERROR_EMPTY;

    /* Copy minimum element (at root) */
    if (entry) {
        *entry = heap->entries[0];
    }

    /* Mark node as no longer in heap */
    heap->positions[heap->entries[0].node] = SH_HEAP_INVALID_POS;

    /* Move last element to root and sift down */
    heap->size--;
    if (heap->size > 0) {
        heap->entries[0] = heap->entries[heap->size];
        heap->positions[heap->entries[0].node] = 0;
        sift_down(heap, 0);
    }

    return SH_HEAP_OK;
}

SHHeapStatus sh_heap_peek(const SHHeap *heap, SHHeapEntry *entry)
{
    if (!heap) return SH_HEAP_ERROR_NULL_PARAM;
    if (heap->size == 0) return SH_HEAP_ERROR_EMPTY;
    if (!entry) return SH_HEAP_ERROR_NULL_PARAM;

    *entry = heap->entries[0];
    return SH_HEAP_OK;
}

SHHeapStatus sh_heap_decrease_key(SHHeap *heap, uint32_t node, double new_priority)
{
    if (!heap) return SH_HEAP_ERROR_NULL_PARAM;
    if (node >= heap->num_nodes) return SH_HEAP_ERROR_INVALID_NODE;

    uint32_t pos = heap->positions[node];
    if (pos == SH_HEAP_INVALID_POS) {
        /* Node not in heap, push it instead */
        return sh_heap_push(heap, node, new_priority);
    }

    /* Only decrease, never increase */
    if (new_priority >= heap->entries[pos].priority) {
        return SH_HEAP_OK;
    }

    heap->entries[pos].priority = new_priority;
    sift_up(heap, pos);

    return SH_HEAP_OK;
}

SHHeapStatus sh_heap_push_or_decrease(SHHeap *heap, uint32_t node, double priority)
{
    if (!heap) return SH_HEAP_ERROR_NULL_PARAM;
    if (node >= heap->num_nodes) return SH_HEAP_ERROR_INVALID_NODE;

    if (sh_heap_contains(heap, node)) {
        return sh_heap_decrease_key(heap, node, priority);
    } else {
        return sh_heap_push(heap, node, priority);
    }
}
