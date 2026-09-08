/*
 * sh_completion.c - Thread Completion Signaling Implementation
 */

#include "sh_completion.h"
#include "sh_pal.h"
#include <string.h>
#include <stdint.h>

void sh_completion_init(ShCompletion *comp)
{
    if (!comp) return;

    memset(comp, 0, sizeof(*comp));
    comp->completed = 0;
    comp->cancelled = 0;
    sh_mutex_init(&comp->mutex);
    sh_cond_init(&comp->cond);
}

void sh_completion_cleanup(ShCompletion *comp)
{
    if (!comp) return;

    sh_mutex_destroy(&comp->mutex);
    sh_cond_destroy(&comp->cond);
}

int sh_completion_wait(ShCompletion *comp, int timeout_ms)
{
    if (!comp) return 0;

    /*
     * The deadline is monotonic and recomputed on every pass. sh_cond_timedwait
     * takes a RELATIVE timeout, so re-passing the original one after a spurious
     * wakeup would restart the full wait; taking it from a fixed deadline keeps
     * the total bounded. Monotonic also means a wall-clock adjustment cannot
     * stretch or collapse the wait, which the old CLOCK_REALTIME deadline
     * allowed.
     */
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = sh_monotonic_ms() + (uint64_t)timeout_ms;
    }

    sh_mutex_lock(&comp->mutex);
    while (!comp->completed) {
        if (timeout_ms > 0) {
            uint64_t now = sh_monotonic_ms();
            if (now >= deadline) {
                sh_mutex_unlock(&comp->mutex);
                return 0;  /* Timeout */
            }
            /* 0 means the timeout expired; the predicate is re-checked by the
             * loop, so a signal racing the expiry still wins. */
            if (sh_cond_timedwait(&comp->cond, &comp->mutex,
                                  deadline - now) == 0 && !comp->completed) {
                sh_mutex_unlock(&comp->mutex);
                return 0;  /* Timeout */
            }
        } else {
            sh_cond_wait(&comp->cond, &comp->mutex);
        }
    }
    sh_mutex_unlock(&comp->mutex);
    return 1;  /* Completed */
}

void sh_completion_cancel(ShCompletion *comp)
{
    if (!comp) return;
    comp->cancelled = 1;
}

void sh_completion_signal(ShCompletion *comp)
{
    if (!comp) return;

    sh_mutex_lock(&comp->mutex);
    comp->completed = 1;
    sh_cond_signal(&comp->cond);
    sh_mutex_unlock(&comp->mutex);
}

int sh_completion_is_cancelled(const ShCompletion *comp)
{
    if (!comp) return 0;
    return comp->cancelled;
}
