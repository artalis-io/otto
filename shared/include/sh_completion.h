/*
 * sh_completion.h - Thread Completion Signaling
 *
 * A simple abstraction for signaling completion between threads.
 * Typically used when an HTTP handler thread waits for a worker
 * thread to complete processing.
 *
 * Usage:
 *   // In server-specific work item struct
 *   typedef struct {
 *       // Your request/response fields
 *       int status_code;
 *       char *response;
 *
 *       // Completion signaling (embed at end)
 *       ShCompletion completion;
 *   } MyWorkItem;
 *
 *   // HTTP handler (producer)
 *   MyWorkItem item;
 *   sh_completion_init(&item.completion);
 *   // ... push to work queue ...
 *   if (!sh_completion_wait(&item.completion, 5000)) {
 *       sh_completion_cancel(&item.completion);  // Timeout
 *       return 504;
 *   }
 *   // ... use item.response ...
 *   sh_completion_cleanup(&item.completion);
 *
 *   // Worker (consumer)
 *   if (sh_completion_is_cancelled(&item->completion)) {
 *       return;  // HTTP handler already timed out
 *   }
 *   // ... process request ...
 *   sh_completion_signal(&item->completion);
 */

#ifndef SH_COMPLETION_H
#define SH_COMPLETION_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Completion signaling state.
 * Embed this in your work item struct.
 */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int completed;
    volatile int cancelled;
} ShCompletion;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Initialize completion signaling.
 * Call this before pushing work to the queue.
 *
 * @param comp Completion state to initialize
 */
void sh_completion_init(ShCompletion *comp);

/*
 * Clean up completion signaling resources.
 * Call this after the work item is no longer needed.
 *
 * @param comp Completion state to clean up (may be NULL)
 */
void sh_completion_cleanup(ShCompletion *comp);

/* ============================================================================
 * Producer API (HTTP handler)
 * ============================================================================ */

/*
 * Wait for completion with timeout.
 *
 * @param comp       Completion state
 * @param timeout_ms Maximum time to wait in milliseconds (0 = wait forever)
 * @return 1 if completed, 0 if timeout
 */
int sh_completion_wait(ShCompletion *comp, int timeout_ms);

/*
 * Mark the completion as cancelled.
 * Call this when the HTTP handler times out.
 * Workers should check sh_completion_is_cancelled() and skip processing.
 *
 * @param comp Completion state
 */
void sh_completion_cancel(ShCompletion *comp);

/* ============================================================================
 * Consumer API (Worker thread)
 * ============================================================================ */

/*
 * Signal that work is complete.
 * Call this when the worker finishes processing.
 * This wakes up the HTTP handler waiting in sh_completion_wait().
 *
 * @param comp Completion state
 */
void sh_completion_signal(ShCompletion *comp);

/*
 * Check if the work item was cancelled.
 * Workers should call this before and during processing to avoid
 * wasted work when the HTTP handler already timed out.
 *
 * @param comp Completion state
 * @return 1 if cancelled, 0 if still active
 */
int sh_completion_is_cancelled(const ShCompletion *comp);

#ifdef __cplusplus
}
#endif

#endif /* SH_COMPLETION_H */
