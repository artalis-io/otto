/*
 * sh_pqueue.h - Generic binary heap priority queue for arbitrary data types
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
 *   SHPQueue pqueue;
 *   sh_pqueue_init(&pqueue, sizeof(MyStruct), my_compare);
 *   sh_pqueue_push(&pqueue, &item);
 *   MyStruct top;
 *   sh_pqueue_pop(&pqueue, &top);
 *   sh_pqueue_free(&pqueue);
 *
 * Comparator convention:
 *   Return > 0 if a should be popped before b (higher priority).
 *   Return < 0 if b should be popped before a.
 *   Return 0 if equal priority.
 *
 *   For a max-heap: return (a > b) ? 1 : (a < b) ? -1 : 0
 *   For a min-heap: return (a < b) ? 1 : (a > b) ? -1 : 0
 */

#ifndef SH_PQUEUE_H
#define SH_PQUEUE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Comparator function type.
 * Returns positive if a has higher priority than b. */
typedef int (*SHPQueueCmp)(const void *a, const void *b);

/* Generic binary heap priority queue */
typedef struct {
    void *data;          /* Element storage */
    size_t count;        /* Number of elements */
    size_t capacity;     /* Allocated slots */
    size_t elem_size;    /* Size of each element in bytes */
    SHPQueueCmp cmp;     /* Comparator */
} SHPQueue;

/*
 * Initialize a priority queue. Must call sh_pqueue_free() when done.
 *
 * @param pqueue     Priority queue to initialize (caller-owned, may be on stack)
 * @param elem_size  Size of each element in bytes (e.g., sizeof(MyStruct))
 * @param cmp        Comparator function (must not be NULL)
 */
void sh_pqueue_init(SHPQueue *pqueue, size_t elem_size, SHPQueueCmp cmp);

/*
 * Free priority queue memory. Safe to call on a zero-initialized or already-freed pqueue.
 * Does not free the SHPQueue struct itself (caller-owned).
 */
void sh_pqueue_free(SHPQueue *pqueue);

/*
 * Push an element onto the priority queue.
 *
 * @param pqueue  Priority queue
 * @param elem    Pointer to element to copy in (elem_size bytes)
 * @return        1 on success, 0 on allocation failure
 */
int sh_pqueue_push(SHPQueue *pqueue, const void *elem);

/*
 * Pop the highest-priority element.
 *
 * @param pqueue  Priority queue
 * @param out     Output buffer (elem_size bytes, may be NULL to discard)
 * @return        1 on success, 0 if priority queue is empty
 */
int sh_pqueue_pop(SHPQueue *pqueue, void *out);

/*
 * Peek at the highest-priority element without removing it.
 *
 * @param pqueue  Priority queue
 * @param out     Output buffer (elem_size bytes)
 * @return        1 on success, 0 if priority queue is empty
 */
int sh_pqueue_peek(const SHPQueue *pqueue, void *out);

/*
 * Get the number of elements in the priority queue.
 */
size_t sh_pqueue_count(const SHPQueue *pqueue);

/*
 * Check if the priority queue is empty.
 */
int sh_pqueue_empty(const SHPQueue *pqueue);

/*
 * Remove all elements without freeing memory.
 * Useful for reusing a priority queue across iterations.
 */
void sh_pqueue_clear(SHPQueue *pqueue);

#ifdef __cplusplus
}
#endif

#endif /* SH_PQUEUE_H */
