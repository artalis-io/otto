/*
 * sh_worker_pool.c - Work Queue Consumer Thread Pool Implementation
 */

#include "sh_worker_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

typedef struct {
    int id;
    pthread_t thread;
} Worker;

struct ShWorkerPool {
    ShWorkQueue *queue;
    ShWorkerCallback callback;
    void *ctx;
    int poll_timeout_ms;

    Worker *workers;
    int num_workers;
    volatile int stopping;
};

/* ============================================================================
 * Worker Thread
 * ============================================================================ */

static void *worker_thread_fn(void *arg)
{
    ShWorkerPool *pool = (ShWorkerPool *)arg;

    while (!pool->stopping) {
        ShWorkItem *item = sh_workqueue_pop_timeout(pool->queue, pool->poll_timeout_ms);
        if (!item) continue;

        /* Call user callback - it's responsible for:
         * - Checking expiration/cancellation
         * - Processing the work
         * - Signaling completion
         * - Freeing the item with sh_workqueue_item_free()
         */
        pool->callback(item, pool->ctx);
    }

    return NULL;
}

/* ============================================================================
 * Auto-detect CPU count
 * ============================================================================ */

static int get_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return (int)sysinfo.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (int)n : 4;
#else
    return 4;  /* Fallback */
#endif
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

ShWorkerPool *sh_worker_pool_create(int num_workers, const ShWorkerPoolConfig *cfg)
{
    if (!cfg || !cfg->queue || !cfg->callback) {
        return NULL;
    }

    /* Auto-detect worker count */
    if (num_workers <= 0) {
        num_workers = get_cpu_count();
    }
    if (num_workers > 256) {
        num_workers = 256;  /* Sanity limit */
    }

    ShWorkerPool *pool = calloc(1, sizeof(ShWorkerPool));
    if (!pool) return NULL;

    pool->queue = cfg->queue;
    pool->callback = cfg->callback;
    pool->ctx = cfg->ctx;
    pool->poll_timeout_ms = cfg->poll_timeout_ms > 0 ? cfg->poll_timeout_ms : 100;
    pool->stopping = 0;

    /* Allocate workers */
    pool->workers = calloc(num_workers, sizeof(Worker));
    if (!pool->workers) {
        free(pool);
        return NULL;
    }

    /* Create worker threads */
    pool->num_workers = 0;
    for (int i = 0; i < num_workers; i++) {
        pool->workers[i].id = i;
        if (pthread_create(&pool->workers[i].thread, NULL, worker_thread_fn, pool) != 0) {
            /* Failed to create thread - stop here */
            break;
        }
        pool->num_workers++;
    }

    /* If no threads were created, clean up */
    if (pool->num_workers == 0) {
        free(pool->workers);
        free(pool);
        return NULL;
    }

    return pool;
}

void sh_worker_pool_stop(ShWorkerPool *pool)
{
    if (!pool) return;

    pool->stopping = 1;

    /* Signal the queue to wake up blocked workers */
    if (pool->queue) {
        sh_workqueue_shutdown(pool->queue);
    }
}

void sh_worker_pool_join(ShWorkerPool *pool)
{
    if (!pool) return;

    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->workers[i].thread) {
            pthread_join(pool->workers[i].thread, NULL);
            pool->workers[i].thread = 0;
        }
    }
}

void sh_worker_pool_free(ShWorkerPool *pool)
{
    if (!pool) return;

    free(pool->workers);
    free(pool);
}

/* ============================================================================
 * Accessors
 * ============================================================================ */

int sh_worker_pool_size(const ShWorkerPool *pool)
{
    return pool ? pool->num_workers : 0;
}

ShWorkQueue *sh_worker_pool_queue(const ShWorkerPool *pool)
{
    return pool ? pool->queue : NULL;
}
