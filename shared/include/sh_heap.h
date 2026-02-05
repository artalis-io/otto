/*
 * sh_heap.h - 4-ary min-heap with position tracking
 *
 * A high-performance priority queue implementation optimized for:
 * - Graph algorithms (Dijkstra, A*, bidirectional search)
 * - Work schedulers with priority updates
 *
 * Features:
 * - 4-ary structure for better cache locality vs binary heap
 * - O(1) contains/priority lookup via position tracking
 * - O(log n) push, pop, and decrease_key operations
 * - Reusable via clear() without reallocation
 */

#ifndef SH_HEAP_H
#define SH_HEAP_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_HEAP_OK = 0,
    SH_HEAP_ERROR_NULL_PARAM,
    SH_HEAP_ERROR_OUT_OF_MEMORY,
    SH_HEAP_ERROR_INVALID_NODE,
    SH_HEAP_ERROR_EMPTY
} SHHeapStatus;

/* ============================================================================
 * Heap Entry
 * ============================================================================ */

typedef struct {
    uint32_t node;       /* Node identifier (e.g., graph node index) */
    double priority;     /* Priority value (lower = higher priority) */
} SHHeapEntry;

/* ============================================================================
 * Heap Structure
 * ============================================================================ */

typedef struct SHHeap SHHeap;

/*
 * Create a new min-heap for tracking nodes with IDs in [0, num_nodes).
 * The position tracking array is sized to num_nodes.
 */
SHHeap *sh_heap_create(size_t num_nodes);

/*
 * Free a heap and all its memory.
 */
void sh_heap_free(SHHeap *heap);

/*
 * Clear all entries without freeing memory.
 * Efficient for reusing heap across multiple queries.
 */
void sh_heap_clear(SHHeap *heap);

/* ============================================================================
 * Query Operations - O(1)
 * ============================================================================ */

/*
 * Check if the heap is empty.
 */
int sh_heap_empty(const SHHeap *heap);

/*
 * Get the number of entries in the heap.
 */
size_t sh_heap_size(const SHHeap *heap);

/*
 * Check if a node is currently in the heap.
 */
int sh_heap_contains(const SHHeap *heap, uint32_t node);

/*
 * Get the current priority of a node.
 * Returns INFINITY if node is not in the heap.
 */
double sh_heap_priority(const SHHeap *heap, uint32_t node);

/* ============================================================================
 * Heap Operations - O(log n)
 * ============================================================================ */

/*
 * Push a node with given priority.
 * If node is already in heap, updates priority if new value is lower.
 */
SHHeapStatus sh_heap_push(SHHeap *heap, uint32_t node, double priority);

/*
 * Pop the minimum-priority entry.
 * If entry is non-NULL, the popped entry is copied to it.
 */
SHHeapStatus sh_heap_pop(SHHeap *heap, SHHeapEntry *entry);

/*
 * Peek at the minimum-priority entry without removing it.
 */
SHHeapStatus sh_heap_peek(const SHHeap *heap, SHHeapEntry *entry);

/*
 * Decrease the priority of a node.
 * If node is not in heap, pushes it with the given priority.
 * If new_priority >= current priority, no change is made.
 */
SHHeapStatus sh_heap_decrease_key(SHHeap *heap, uint32_t node, double new_priority);

/*
 * Combined push-or-decrease operation.
 * Convenience wrapper that handles both cases.
 */
SHHeapStatus sh_heap_push_or_decrease(SHHeap *heap, uint32_t node, double priority);

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Sentinel value for "not in heap" */
#define SH_HEAP_INVALID_POS UINT32_MAX

/* Very large priority value (effectively infinite) */
#define SH_HEAP_INF 1e308

#endif /* SH_HEAP_H */
