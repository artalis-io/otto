/*
 * sh_workqueue.c - Thread-Safe Bounded Work Queue Implementation
 */

#include "sh_workqueue.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
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
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;  /* Signaled when items are available */
    pthread_cond_t not_full;   /* Signaled when space is available (optional) */
    int shutdown;              /* Set to 1 during shutdown */

    /* Statistics */
    uint64_t total_pushed;
    uint64_t total_popped;
    uint64_t total_dropped;
    uint64_t total_expired;
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static double get_time_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* Convert timeout_ms to absolute timespec for pthread_cond_timedwait */
static void timeout_to_abstime(int timeout_ms, struct timespec *ts)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    ts->tv_sec = tv.tv_sec + timeout_ms / 1000;
    ts->tv_nsec = tv.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;

    /* Handle nanosecond overflow */
    if (ts->tv_nsec >= 1000000000) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000;
    }
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

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        free(queue);
        return NULL;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        return NULL;
    }

    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        free(queue);
        return NULL;
    }

    return queue;
}

void sh_workqueue_shutdown(ShWorkQueue *queue)
{
    if (!queue) return;

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    /* Wake up all waiting consumers */
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
}

void sh_workqueue_free(ShWorkQueue *queue)
{
    if (!queue) return;

    /* Free any remaining items */
    pthread_mutex_lock(&queue->mutex);
    while (queue->count > 0) {
        ShWorkItem *item = &queue->items[queue->tail];
        free(item->data);
        item->data = NULL;
        queue->tail = (queue->tail + 1) % queue->capacity;
        queue->count--;
    }
    pthread_mutex_unlock(&queue->mutex);

    pthread_cond_destroy(&queue->not_full);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    free(queue);
}

/* ============================================================================
 * Producer API
 * ============================================================================ */

int sh_workqueue_push(ShWorkQueue *queue, const ShWorkItem *item)
{
    if (!queue || !item) return 0;

    pthread_mutex_lock(&queue->mutex);

    /* Check if full or shutting down */
    if (queue->count >= queue->capacity || queue->shutdown) {
        queue->total_dropped++;
        pthread_mutex_unlock(&queue->mutex);
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
    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

int sh_workqueue_try_push(ShWorkQueue *queue, const ShWorkItem *item, double *pressure)
{
    if (!queue || !item) {
        if (pressure) *pressure = 1.0;
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);

    if (pressure) {
        *pressure = (double)queue->count / (double)queue->capacity;
    }

    /* Check if full or shutting down */
    if (queue->count >= queue->capacity || queue->shutdown) {
        queue->total_dropped++;
        pthread_mutex_unlock(&queue->mutex);
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

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

/* ============================================================================
 * Consumer API
 * ============================================================================ */

ShWorkItem *sh_workqueue_pop(ShWorkQueue *queue)
{
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    /* Wait for item or shutdown */
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    /* Check for shutdown with empty queue */
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Allocate result (we need to return a pointer the caller can free) */
    ShWorkItem *result = malloc(sizeof(ShWorkItem));
    if (!result) {
        pthread_mutex_unlock(&queue->mutex);
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

    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);
    return result;
}

ShWorkItem *sh_workqueue_pop_timeout(ShWorkQueue *queue, int timeout_ms)
{
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->mutex);

    /* Non-blocking check */
    if (timeout_ms == 0) {
        if (queue->count == 0 || queue->shutdown) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
    } else {
        /* Wait with timeout */
        struct timespec abstime;
        timeout_to_abstime(timeout_ms, &abstime);

        while (queue->count == 0 && !queue->shutdown) {
            int rc = pthread_cond_timedwait(&queue->not_empty, &queue->mutex, &abstime);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&queue->mutex);
                return NULL;
            }
        }
    }

    /* Check for shutdown with empty queue */
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return NULL;
    }

    /* Allocate result */
    ShWorkItem *result = malloc(sizeof(ShWorkItem));
    if (!result) {
        pthread_mutex_unlock(&queue->mutex);
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

    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);
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

    pthread_mutex_lock(&queue->mutex);
    stats->current_depth = queue->count;
    stats->max_capacity = queue->capacity;
    stats->total_pushed = queue->total_pushed;
    stats->total_popped = queue->total_popped;
    stats->total_dropped = queue->total_dropped;
    stats->total_expired = queue->total_expired;
    stats->timeout_sec = queue->timeout_sec;
    pthread_mutex_unlock(&queue->mutex);
}

size_t sh_workqueue_depth(ShWorkQueue *queue)
{
    if (!queue) return 0;

    pthread_mutex_lock(&queue->mutex);
    size_t depth = queue->count;
    pthread_mutex_unlock(&queue->mutex);

    return depth;
}

int sh_workqueue_full(ShWorkQueue *queue)
{
    if (!queue) return 1;

    pthread_mutex_lock(&queue->mutex);
    int full = (queue->count >= queue->capacity);
    pthread_mutex_unlock(&queue->mutex);

    return full;
}
