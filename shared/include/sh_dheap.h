/*
 * sh_dheap.h - Generic binary heap for arbitrary data types
 *
 * A priority queue that stores elements of any fixed size, ordered
 * by a user-provided comparator. Unlike sh_heap (which tracks graph
 * node IDs), this stores complete data elements and supports any type.
 *
 * Features:
 *  - Arbitrary element size (void* storage)
 *  - User-provided comparator controls ordering (max-heap or min-heap)
 *  - Stack-allocatable via init/free (no mandatory heap allocation)
 *  - Dynamic growth with amortized O(1) push
 *  - O(log n) push and pop
 *
 * Usage:
 *   SHDHeap heap;
 *   sh_dheap_init(&heap, sizeof(MyStruct), my_compare);
 *   sh_dheap_push(&heap, &item);
 *   MyStruct top;
 *   sh_dheap_pop(&heap, &top);
 *   sh_dheap_free(&heap);
 *
 * Comparator convention:
 *   Return > 0 if a should be popped before b (higher priority).
 *   Return < 0 if b should be popped before a.
 *   Return 0 if equal priority.
 *
 *   For a max-heap: return (a > b) ? 1 : (a < b) ? -1 : 0
 *   For a min-heap: return (a < b) ? 1 : (a > b) ? -1 : 0
 */

#ifndef SH_DHEAP_H
#define SH_DHEAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Comparator function type.
 * Returns positive if a has higher priority than b. */
typedef int (*SHDHeapCmp)(const void *a, const void *b);

/* Generic binary heap */
typedef struct {
    void *data;          /* Element storage */
    size_t count;        /* Number of elements */
    size_t capacity;     /* Allocated slots */
    size_t elem_size;    /* Size of each element in bytes */
    SHDHeapCmp cmp;      /* Comparator */
} SHDHeap;

/*
 * Initialize a heap. Must call sh_dheap_free() when done.
 *
 * @param heap       Heap to initialize (caller-owned, may be on stack)
 * @param elem_size  Size of each element in bytes (e.g., sizeof(MyStruct))
 * @param cmp        Comparator function (must not be NULL)
 */
void sh_dheap_init(SHDHeap *heap, size_t elem_size, SHDHeapCmp cmp);

/*
 * Free heap memory. Safe to call on a zero-initialized or already-freed heap.
 * Does not free the SHDHeap struct itself (caller-owned).
 */
void sh_dheap_free(SHDHeap *heap);

/*
 * Push an element onto the heap.
 *
 * @param heap  Heap
 * @param elem  Pointer to element to copy in (elem_size bytes)
 * @return      1 on success, 0 on allocation failure
 */
int sh_dheap_push(SHDHeap *heap, const void *elem);

/*
 * Pop the highest-priority element.
 *
 * @param heap  Heap
 * @param out   Output buffer (elem_size bytes, may be NULL to discard)
 * @return      1 on success, 0 if heap is empty
 */
int sh_dheap_pop(SHDHeap *heap, void *out);

/*
 * Peek at the highest-priority element without removing it.
 *
 * @param heap  Heap
 * @param out   Output buffer (elem_size bytes)
 * @return      1 on success, 0 if heap is empty
 */
int sh_dheap_peek(const SHDHeap *heap, void *out);

/*
 * Get the number of elements in the heap.
 */
size_t sh_dheap_count(const SHDHeap *heap);

/*
 * Check if the heap is empty.
 */
int sh_dheap_empty(const SHDHeap *heap);

/*
 * Remove all elements without freeing memory.
 * Useful for reusing a heap across iterations.
 */
void sh_dheap_clear(SHDHeap *heap);

#ifdef __cplusplus
}
#endif

#endif /* SH_DHEAP_H */
