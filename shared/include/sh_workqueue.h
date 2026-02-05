/*
 * sh_workqueue.h - Thread-Safe Bounded Work Queue
 *
 * A producer-consumer queue for handling HTTP requests with:
 * - Bounded size for backpressure (returns failure when full)
 * - Request timeout (drop stale items that clients abandoned)
 * - Graceful shutdown support
 * - Statistics for monitoring
 *
 * Usage:
 *   // Create queue: max 1000 items, 5 second timeout
 *   ShWorkQueue *queue = sh_workqueue_create(1000, 5.0);
 *
 *   // Producer (HTTP handler thread):
 *   ShWorkItem item = { .data = request_copy, .data_len = len, .user_ctx = conn };
 *   if (!sh_workqueue_push(queue, &item)) {
 *       return 503;  // Queue full - backpressure
 *   }
 *
 *   // Consumer (worker thread):
 *   ShWorkItem *item;
 *   while ((item = sh_workqueue_pop(queue)) != NULL) {
 *       if (!sh_workqueue_item_expired(queue, item)) {
 *           process_request(item);
 *       }
 *       sh_workqueue_item_free(item);
 *   }
 *
 *   // Shutdown
 *   sh_workqueue_shutdown(queue);
 *   sh_workqueue_free(queue);
 */

#ifndef SH_WORKQUEUE_H
#define SH_WORKQUEUE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Work item representing a queued request.
 * The queue takes ownership of 'data' - it will be freed when the item is freed.
 */
typedef struct {
    void *data;           /* Request data (owned by queue after push) */
    size_t data_len;      /* Data length in bytes */
    void *user_ctx;       /* User context (connection handle, etc.) - NOT owned */
    double enqueue_time;  /* When item was queued (set by sh_workqueue_push) */
    volatile int cancelled; /* Set to 1 when HTTP handler times out */
} ShWorkItem;

/*
 * Work queue instance.
 * Opaque handle - use sh_workqueue_create() to construct.
 */
typedef struct ShWorkQueue ShWorkQueue;

/*
 * Queue statistics for monitoring.
 */
typedef struct {
    size_t current_depth;    /* Current number of items in queue */
    size_t max_capacity;     /* Maximum queue capacity */
    uint64_t total_pushed;   /* Total items successfully pushed */
    uint64_t total_popped;   /* Total items popped (including expired) */
    uint64_t total_dropped;  /* Items dropped due to queue full */
    uint64_t total_expired;  /* Items dropped due to timeout */
    uint64_t total_cancelled; /* Items cancelled before worker completion */
    double timeout_sec;      /* Configured timeout */
} ShWorkQueueStats;

/* ============================================================================
 * Queue Lifecycle
 * ============================================================================ */

/*
 * Create a new work queue.
 *
 * @param max_items   Maximum number of items the queue can hold
 * @param timeout_sec Request timeout in seconds (0 = no timeout)
 * @return New queue, or NULL on allocation failure
 *
 * OWNERSHIP: Caller owns the returned queue and must call sh_workqueue_free().
 */
ShWorkQueue *sh_workqueue_create(size_t max_items, double timeout_sec);

/*
 * Signal shutdown to all waiting consumers.
 * After this call, sh_workqueue_pop() will return NULL for all waiting threads.
 * Call this before sh_workqueue_free() to cleanly stop worker threads.
 *
 * @param queue Queue to shut down
 */
void sh_workqueue_shutdown(ShWorkQueue *queue);

/*
 * Free a work queue and all remaining items.
 * Call sh_workqueue_shutdown() first to stop worker threads.
 *
 * @param queue Queue to free (may be NULL)
 */
void sh_workqueue_free(ShWorkQueue *queue);

/* ============================================================================
 * Producer API (HTTP handler thread)
 * ============================================================================ */

/*
 * Push a work item onto the queue.
 * The queue takes ownership of item->data (will be freed when item is freed).
 * The enqueue_time is set automatically by this function.
 *
 * @param queue Queue to push to
 * @param item  Work item to push (data is copied, data pointer is taken)
 * @return 1 if pushed successfully, 0 if queue is full (caller should return 503)
 *
 * Thread-safe: Can be called from multiple producer threads.
 */
int sh_workqueue_push(ShWorkQueue *queue, const ShWorkItem *item);

/*
 * Try to push without blocking, with a hint about queue pressure.
 *
 * @param queue     Queue to push to
 * @param item      Work item to push
 * @param pressure  Output: current queue depth / max capacity (0.0 to 1.0)
 * @return 1 if pushed, 0 if full
 */
int sh_workqueue_try_push(ShWorkQueue *queue, const ShWorkItem *item, double *pressure);

/* ============================================================================
 * Consumer API (Worker thread)
 * ============================================================================ */

/*
 * Pop a work item from the queue, blocking if empty.
 * Returns NULL if the queue is shut down.
 *
 * @param queue Queue to pop from
 * @return Work item (caller must call sh_workqueue_item_free), or NULL on shutdown
 *
 * Thread-safe: Can be called from multiple consumer threads.
 */
ShWorkItem *sh_workqueue_pop(ShWorkQueue *queue);

/*
 * Pop with timeout - returns NULL if no item available within timeout.
 *
 * @param queue      Queue to pop from
 * @param timeout_ms Maximum time to wait in milliseconds (0 = non-blocking)
 * @return Work item, or NULL on timeout/shutdown
 */
ShWorkItem *sh_workqueue_pop_timeout(ShWorkQueue *queue, int timeout_ms);

/*
 * Check if a work item has expired (age > queue timeout).
 * Expired items should be dropped without processing.
 *
 * @param queue Queue the item came from
 * @param item  Item to check
 * @return 1 if expired, 0 if still valid
 */
int sh_workqueue_item_expired(ShWorkQueue *queue, const ShWorkItem *item);

/*
 * Get the age of a work item in seconds.
 *
 * @param item Item to check
 * @return Age in seconds since enqueue
 */
double sh_workqueue_item_age(const ShWorkItem *item);

/*
 * Mark a work item as cancelled.
 * Call this when the HTTP handler times out before the worker completes.
 * The worker should periodically check sh_workqueue_item_cancelled() and
 * abort early if the result will be discarded anyway.
 *
 * This version also increments the queue's total_cancelled counter for stats.
 *
 * Thread-safe: Can be called from any thread.
 *
 * @param queue Queue the item came from (for stats tracking, may be NULL)
 * @param item  Item to cancel
 */
void sh_workqueue_item_cancel(ShWorkQueue *queue, ShWorkItem *item);

/*
 * Check if a work item was cancelled.
 * Workers should call this periodically during long computations and
 * abort early if cancelled (the HTTP handler already returned 504).
 *
 * Thread-safe: Can be called from any thread (uses atomic read).
 *
 * @param item Item to check
 * @return 1 if cancelled, 0 if still active
 */
int sh_workqueue_item_cancelled(const ShWorkItem *item);

/*
 * Free a work item after processing.
 * This frees item->data and the item struct itself.
 *
 * @param item Item to free (may be NULL)
 */
void sh_workqueue_item_free(ShWorkItem *item);

/* ============================================================================
 * Monitoring
 * ============================================================================ */

/*
 * Get queue statistics.
 *
 * @param queue Queue to query
 * @param stats Output statistics
 */
void sh_workqueue_stats(ShWorkQueue *queue, ShWorkQueueStats *stats);

/*
 * Get current queue depth (number of items waiting).
 * Useful for load monitoring without full stats.
 *
 * @param queue Queue to query
 * @return Current depth
 */
size_t sh_workqueue_depth(ShWorkQueue *queue);

/*
 * Check if queue is full.
 *
 * @param queue Queue to check
 * @return 1 if full, 0 otherwise
 */
int sh_workqueue_full(ShWorkQueue *queue);

#ifdef __cplusplus
}
#endif

#endif /* SH_WORKQUEUE_H */
