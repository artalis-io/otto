/*
 * sh_perf.c - Lightweight monotonic timing helpers
 */

#include "sh_perf.h"
#include <time.h>

double sh_perf_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

