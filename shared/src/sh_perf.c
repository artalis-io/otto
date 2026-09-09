/*
 * sh_perf.c - Lightweight monotonic timing helpers
 */

#include "sh_perf.h"
#include "sh_pal.h"

double sh_perf_now_ms(void)
{
    return (double)sh_monotonic_ns() / 1.0e6;
}

