/*
 * sh_worker_pool.h - Work Queue Consumer Thread Pool
 *
 * A simple abstraction for creating worker threads that consume from a
 * ShWorkQueue. Handles thread lifecycle (creation, shutdown, join) and
 * the standard poll loop pattern.
 *
 * Usage:
 *   // Define a callback to process work items
 *   void my_worker_callback(ShWorkItem *item, void *ctx) {
 *       MyWorkItem *work = (MyWorkItem *)item->user_ctx;
 *       if (!work) return;
 *
 *       // Check for cancellation
 *       if (sh_completion_is_cancelled(&work->completion)) {
 *           sh_completion_signal(&work->completion);
 *           return;
 *       }
 *
 *       // Process the work
 *       do_work(work);
 *
 *       // Signal completion
 *       sh_completion_signal(&work->completion);
 *   }
 *
 *   // Create pool
 *   ShWorkerPoolConfig cfg = {
 *       .queue = my_queue,
 *       .callback = my_worker_callback,
 *       .ctx = my_context
 *   };
 *   ShWorkerPool *pool = sh_worker_pool_create(0, &cfg);  // 0 = auto-detect
 *
 *   // ... use the queue ...
 *
 *   // Shutdown
 *   sh_worker_pool_stop(pool);   // Signals queue shutdown
 *   sh_worker_pool_join(pool);   // Waits for threads
 *   sh_worker_pool_free(pool);   // Frees resources
 */

#ifndef SH_WORKER_POOL_H
#define SH_WORKER_POOL_H

#include "sh_workqueue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Worker callback function.
 * Called for each work item popped from the queue.
 *
 * The callback is responsible for:
 * - Extracting server-specific data from item->user_ctx
 * - Checking sh_workqueue_item_expired() or item->cancelled
 * - Processing the work (if not expired/cancelled)
 * - Signaling completion to any waiting thread
 *
 * The pool does NOT free the work item - the callback must call
 * sh_workqueue_item_free(item) when done with the item.
 *
 * @param item  Work item from queue (never NULL)
 * @param ctx   User context from ShWorkerPoolConfig
 */
typedef void (*ShWorkerCallback)(ShWorkItem *item, void *ctx);

/*
 * Worker pool configuration.
 */
typedef struct {
    ShWorkQueue *queue;         /* Work queue to consume from (required) */
    ShWorkerCallback callback;  /* Called for each work item (required) */
    void *ctx;                  /* User context passed to callback */
    int poll_timeout_ms;        /* Timeout for sh_workqueue_pop_timeout (default: 100) */
} ShWorkerPoolConfig;

/*
 * Worker pool handle.
 */
typedef struct ShWorkerPool ShWorkerPool;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create a worker pool with the specified number of threads.
 *
 * @param num_workers Number of worker threads (0 = auto-detect CPU count)
 * @param cfg         Pool configuration
 * @return Pool handle, or NULL on error
 */
ShWorkerPool *sh_worker_pool_create(int num_workers, const ShWorkerPoolConfig *cfg);

/*
 * Signal the pool to stop.
 * This calls sh_workqueue_shutdown() on the queue.
 * Workers will finish their current item and exit.
 *
 * @param pool Pool to stop (may be NULL)
 */
void sh_worker_pool_stop(ShWorkerPool *pool);

/*
 * Wait for all worker threads to exit.
 * Call sh_worker_pool_stop() first.
 *
 * @param pool Pool to join (may be NULL)
 */
void sh_worker_pool_join(ShWorkerPool *pool);

/*
 * Free the worker pool.
 * Call sh_worker_pool_stop() and sh_worker_pool_join() first.
 *
 * @param pool Pool to free (may be NULL)
 */
void sh_worker_pool_free(ShWorkerPool *pool);

/* ============================================================================
 * Accessors
 * ============================================================================ */

/*
 * Get the number of worker threads.
 *
 * @param pool Pool to query
 * @return Number of workers, or 0 if pool is NULL
 */
int sh_worker_pool_size(const ShWorkerPool *pool);

/*
 * Get the associated work queue.
 *
 * @param pool Pool to query
 * @return Work queue, or NULL if pool is NULL
 */
ShWorkQueue *sh_worker_pool_queue(const ShWorkerPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* SH_WORKER_POOL_H */
