/*
 * sh_completion.c - Thread Completion Signaling Implementation
 */

#include "sh_completion.h"
#include <string.h>
#include <time.h>
#include <errno.h>

void sh_completion_init(ShCompletion *comp)
{
    if (!comp) return;

    memset(comp, 0, sizeof(*comp));
    comp->completed = 0;
    comp->cancelled = 0;
    pthread_mutex_init(&comp->mutex, NULL);
    pthread_cond_init(&comp->cond, NULL);
}

void sh_completion_cleanup(ShCompletion *comp)
{
    if (!comp) return;

    pthread_mutex_destroy(&comp->mutex);
    pthread_cond_destroy(&comp->cond);
}

int sh_completion_wait(ShCompletion *comp, int timeout_ms)
{
    if (!comp) return 0;

    /* Calculate absolute timeout */
    struct timespec abstime;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &abstime);
        abstime.tv_sec += timeout_ms / 1000;
        abstime.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (abstime.tv_nsec >= 1000000000L) {
            abstime.tv_sec++;
            abstime.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&comp->mutex);
    while (!comp->completed) {
        int rc;
        if (timeout_ms > 0) {
            rc = pthread_cond_timedwait(&comp->cond, &comp->mutex, &abstime);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&comp->mutex);
                return 0;  /* Timeout */
            }
        } else {
            rc = pthread_cond_wait(&comp->cond, &comp->mutex);
        }
        (void)rc;  /* Ignore spurious wakeups, loop will check completed */
    }
    pthread_mutex_unlock(&comp->mutex);
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

    pthread_mutex_lock(&comp->mutex);
    comp->completed = 1;
    pthread_cond_signal(&comp->cond);
    pthread_mutex_unlock(&comp->mutex);
}

int sh_completion_is_cancelled(const ShCompletion *comp)
{
    if (!comp) return 0;
    return comp->cancelled;
}
