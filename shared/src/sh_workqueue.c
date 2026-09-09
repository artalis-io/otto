/*
 * sh_workqueue.c - Thread-Safe Bounded Work Queue Implementation
 */

#include "sh_workqueue.h"
#include "sh_pal.h"
#include <stdlib.h>
#include <string.h>
#include "sh_pal.h"
#include <errno.h>

/* ============================================================================
 * Internal Types
 * ============================================================================ */

struct ShWorkQueue {
    /* Circular buffer */
    ShWorkItem *items;       /* Array of items */
    size_t capacity;         /* Max items */
    size_t head;             /* Next position to push */
    size_t tail;             /* Next position to pop */
    size_t count;            /* Current number of items */

    /* Configuration */
    double timeout_sec;      /* Request timeout (0 = no timeout) */

    /* Synchronization */
    ShMutex mutex;
    ShCond not_empty;  /* Signaled when items are available */
    ShCond not_full;   /* Signaled when space is available (optional) */
    int shutdown;              /* Set to 1 during shutdown */

    /* Statistics */
    uint64_t total_pushed;
    uint64_t total_popped;
    uint64_t total_dropped;
    uint64_t total_expired;
    uint64_t total_cancelled;
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static double get_time_seconds(void)
{
    return (double)sh_wall_ns() / 1.0e9;
}

/* ============================================================================
 * Queue Lifecycle
 * ============================================================================ */

ShWorkQueue *sh_workqueue_create(size_t max_items, double timeout_sec)
{
    if (max_items == 0) return NULL;

    ShWorkQueue *queue = calloc(1, sizeof(ShWorkQueue));
    if (!queue) return NULL;

    queue->items = calloc(max_items, sizeof(ShWorkItem));
    if (!queue->items) {
        free(queue);
        return NULL;
    }

    queue->capacity = max_items;
    queue->timeout_sec = timeout_sec;

    if (sh_mutex_init(&queue->mutex) != 0) {
        free(queue->items);
        free(queue);
        return NULL;
    }

    if (sh_cond_init(&queue->not_empty) != 0) {
        sh_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        return NULL;
    }

    if (sh_cond_init(&queue->not_full) != 0) {
        sh_cond_destroy(&queue->not_empty);
        sh_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        return NULL;
    }

    return queue;
}

void sh_workqueue_shutdown(ShWorkQueue *queue)
{
    if (!queue) return;

    sh_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    /* Wake up all waiting consumers */
    sh_cond_broadcast(&queue->not_empty);
    sh_cond_broadcast(&queue->not_full);
    sh_mutex_unlock(&queue->mutex);
}

void sh_workqueue_free(ShWorkQueue *queue)
{
    if (!queue) return;

    /* Free any remaining items */
    sh_mutex_lock(&queue->mutex);
    while (queue->count > 0) {
        ShWorkItem *item = &queue->items[queue->tail];
        free(item->data);
        item->data = NULL;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count--;
    }
    sh_mutex_unlock(&queue->mutex);

    sh_cond_destroy(&queue->not_full);
    sh_cond_destroy(&queue->not_empty);
    sh_mutex_destroy(&queue->mutex);
    free(queue->items);
    free(queue);
}

/* ============================================================================
 * Producer API
 * ============================================================================ */

int sh_workqueue_push(ShWorkQueue *queue, const ShWorkItem *item)
{
    if (!queue || !item) return 0;

    sh_mutex_lock(&queue->mutex);

    /* Check if full or shutting down */
    if (queue->count >= queue->capacity || queue->shutdown) {
        queue->total_dropped++;
        sh_mutex_unlock(&queue->mutex);
        return 0;
    }

    /* Copy item into queue */
    ShWorkItem *slot = &queue->items[queue->head];
    slot->data = item->data;  /* Take ownership */
    slot->data_len = item->data_len;
    slot->user_ctx = item->user_ctx;
    slot->enqueue_time = get_time_seconds();

    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;
    queue->total_pushed++;

    /* Wake up one waiting consumer */
    sh_cond_signal(&queue->not_empty);

    sh_mutex_unlock(&queue->mutex);
    return 1;
}

int sh_workqueue_try_push(ShWorkQueue *queue, const ShWorkItem *item, double *pressure)
{
    if (!queue || !item) {
        if (pressure) *pressure = 1.0;
        return 0;
    }

    sh_mutex_lock(&queue->mutex);

    if (pressure) {
        *pressure = (double)queue->count / (double)queue->capacity;
    }

    /* Check if full or shutting down */
    if (queue->count >= queue->capacity || queue->shutdown) {
        queue->total_dropped++;
        sh_mutex_unlock(&queue->mutex);
        return 0;
    }

    /* Copy item into queue */
    ShWorkItem *slot = &queue->items[queue->head];
    slot->data = item->data;
    slot->data_len = item->data_len;
    slot->user_ctx = item->user_ctx;
    slot->enqueue_time = get_time_seconds();

    queue->head = (queue->head + 1) % queue->capacity;
    queue->count++;
    queue->total_pushed++;

    sh_cond_signal(&queue->not_empty);

    sh_mutex_unlock(&queue->mutex);
    return 1;
}

/* ============================================================================
 * Consumer API
 * ============================================================================ */

ShWorkItem *sh_workqueue_pop(ShWorkQueue *queue)
{
    if (!queue) return NULL;

    sh_mutex_lock(&queue->mutex);

    /* Wait for item or shutdown */
    while (queue->count == 0 && !queue->shutdown) {
        sh_cond_wait(&queue->not_empty, &queue->mutex);
    }

    /* Check for shutdown with empty queue */
    if (queue->count == 0) {
        sh_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Allocate result (we need to return a pointer the caller can free) */
    ShWorkItem *result = malloc(sizeof(ShWorkItem));
    if (!result) {
        sh_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Copy item from queue */
    ShWorkItem *slot = &queue->items[queue->tail];
    *result = *slot;
    slot->data = NULL;  /* Ownership transferred to result */

    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;
    queue->total_popped++;

    /* Check if expired */
    if (queue->timeout_sec > 0) {
        double age = get_time_seconds() - result->enqueue_time;
        if (age > queue->timeout_sec) {
            queue->total_expired++;
        }
    }

    sh_cond_signal(&queue->not_full);

    sh_mutex_unlock(&queue->mutex);
    return result;
}

ShWorkItem *sh_workqueue_pop_timeout(ShWorkQueue *queue, int timeout_ms)
{
    if (!queue) return NULL;

    sh_mutex_lock(&queue->mutex);

    /* Non-blocking check */
    if (timeout_ms == 0) {
        if (queue->count == 0 || queue->shutdown) {
            sh_mutex_unlock(&queue->mutex);
            return NULL;
        }
    } else {
        /*
         * Deadline held fixed, remaining time recomputed each pass:
         * sh_cond_timedwait takes a RELATIVE timeout, so re-passing the
         * original one after a spurious wakeup would restart the full wait.
         * Monotonic, so a wall-clock adjustment cannot change it.
         */
        uint64_t deadline = sh_monotonic_ms() + (uint64_t)timeout_ms;

        while (queue->count == 0 && !queue->shutdown) {
            uint64_t now = sh_monotonic_ms();
            if (now >= deadline) {
                sh_mutex_unlock(&queue->mutex);
                return NULL;
            }
            if (sh_cond_timedwait(&queue->not_empty, &queue->mutex,
                                  deadline - now) == 0 &&
                queue->count == 0 && !queue->shutdown) {
                sh_mutex_unlock(&queue->mutex);
                return NULL;
            }
        }
    }

    /* Check for shutdown with empty queue */
    if (queue->count == 0) {
        sh_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Allocate result */
    ShWorkItem *result = malloc(sizeof(ShWorkItem));
    if (!result) {
        sh_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Copy item from queue */
    ShWorkItem *slot = &queue->items[queue->tail];
    *result = *slot;
    slot->data = NULL;

    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count--;
    queue->total_popped++;

    if (queue->timeout_sec > 0) {
        double age = get_time_seconds() - result->enqueue_time;
        if (age > queue->timeout_sec) {
            queue->total_expired++;
        }
    }

    sh_cond_signal(&queue->not_full);

    sh_mutex_unlock(&queue->mutex);
    return result;
}

int sh_workqueue_item_expired(ShWorkQueue *queue, const ShWorkItem *item)
{
    if (!queue || !item) return 0;
    if (queue->timeout_sec <= 0) return 0;

    double age = get_time_seconds() - item->enqueue_time;
    return age > queue->timeout_sec;
}

double sh_workqueue_item_age(const ShWorkItem *item)
{
    if (!item) return 0;
    return get_time_seconds() - item->enqueue_time;
}

void sh_workqueue_item_cancel(ShWorkQueue *queue, ShWorkItem *item)
{
    if (!item) return;

    /* Use atomic store for thread safety (volatile + direct assignment is
     * sufficient for single-word writes on most architectures, but we use
     * __atomic_store_n for portability and explicit memory ordering) */
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(&item->cancelled, 1, __ATOMIC_RELEASE);
#else
    item->cancelled = 1;
#endif

    /* Update stats if queue provided */
    if (queue) {
        sh_mutex_lock(&queue->mutex);
        queue->total_cancelled++;
        sh_mutex_unlock(&queue->mutex);
    }
}

int sh_workqueue_item_cancelled(const ShWorkItem *item)
{
    if (!item) return 0;
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(&item->cancelled, __ATOMIC_ACQUIRE);
#else
    return item->cancelled;
#endif
}

void sh_workqueue_item_free(ShWorkItem *item)
{
    if (!item) return;
    free(item->data);
    free(item);
}

/* ============================================================================
 * Monitoring
 * ============================================================================ */

void sh_workqueue_stats(ShWorkQueue *queue, ShWorkQueueStats *stats)
{
    if (!queue || !stats) return;

    sh_mutex_lock(&queue->mutex);
    stats->current_depth = queue->count;
    stats->max_capacity = queue->capacity;
    stats->total_pushed = queue->total_pushed;
    stats->total_popped = queue->total_popped;
    stats->total_dropped = queue->total_dropped;
    stats->total_expired = queue->total_expired;
    stats->total_cancelled = queue->total_cancelled;
    stats->timeout_sec = queue->timeout_sec;
    sh_mutex_unlock(&queue->mutex);
}

size_t sh_workqueue_depth(ShWorkQueue *queue)
{
    if (!queue) return 0;

    sh_mutex_lock(&queue->mutex);
    size_t depth = queue->count;
    sh_mutex_unlock(&queue->mutex);

    return depth;
}

int sh_workqueue_full(ShWorkQueue *queue)
{
    if (!queue) return 1;

    sh_mutex_lock(&queue->mutex);
    int full = (queue->count >= queue->capacity);
    sh_mutex_unlock(&queue->mutex);

    return full;
}
