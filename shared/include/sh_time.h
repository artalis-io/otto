/*
 * sh_time.h - Monotonic clock utilities
 *
 * Provides a portable monotonic clock for timing, deadlines, and benchmarks.
 * All functions return seconds as double for easy arithmetic.
 */

#ifndef SH_TIME_H
#define SH_TIME_H

#include "sh_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return current monotonic time in seconds (fractional).
   Delegates to the PAL: clock_gettime is POSIX-only and MSVC has no such
   function, so this header could not be included at all when building with
   it -- which quietly kept surge, and ralph's LAP suite, off that compiler.
   Monotonic clocks are not affected by NTP adjustments or wall-clock changes.
   Use for elapsed time measurement, deadlines, and benchmarks. */
static inline double sh_monotonic_seconds(void) {
    return (double)sh_monotonic_ns() / 1.0e9;
}

#ifdef __cplusplus
}
#endif

#endif /* SH_TIME_H */
