/*
 * sh_time.h - Monotonic clock utilities
 *
 * Provides a portable monotonic clock for timing, deadlines, and benchmarks.
 * All functions return seconds as double for easy arithmetic.
 */

#ifndef SH_TIME_H
#define SH_TIME_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return current monotonic time in seconds (fractional).
   Monotonic clocks are not affected by NTP adjustments or wall-clock changes.
   Use for elapsed time measurement, deadlines, and benchmarks. */
static inline double sh_monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#ifdef __cplusplus
}
#endif

#endif /* SH_TIME_H */
