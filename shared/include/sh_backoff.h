/*
 * sh_backoff.h - Exponential Backoff with Jitter
 *
 * Provides exponential backoff calculations for retry logic.
 * Supports both stateful iterator and stateless calculation APIs.
 *
 * Usage (stateful):
 *   ShBackoff backoff;
 *   ShBackoffConfig config = SH_BACKOFF_DEFAULT_CONFIG;
 *   sh_backoff_init(&backoff, &config);
 *
 *   while (sh_backoff_has_retries(&backoff)) {
 *       double delay = sh_backoff_next(&backoff);
 *       usleep((useconds_t)(delay * 1000));  // Sleep in microseconds
 *       if (try_request()) break;
 *   }
 *
 * Usage (stateless):
 *   double delay = sh_backoff_calculate(&config, attempt);
 */

#ifndef SH_BACKOFF_H
#define SH_BACKOFF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Backoff configuration.
 */
typedef struct {
    double base_delay_ms;   /* Initial delay in milliseconds (default: 100) */
    double max_delay_ms;    /* Maximum delay cap (default: 10000) */
    int max_retries;        /* Maximum retry attempts (default: 5) */
    double jitter_factor;   /* Jitter multiplier 0.0-1.0 (default: 0.5) */
} ShBackoffConfig;

/*
 * Default configuration: 100ms base, 10s max, 5 retries, 50% jitter
 */
#define SH_BACKOFF_DEFAULT_CONFIG { \
    .base_delay_ms = 100.0, \
    .max_delay_ms = 10000.0, \
    .max_retries = 5, \
    .jitter_factor = 0.5 \
}

/*
 * Backoff iterator state.
 */
typedef struct {
    ShBackoffConfig config;
    int attempt;            /* Current attempt (0-indexed, incremented by sh_backoff_next) */
    double last_delay_ms;   /* Last computed delay */
    uint64_t seed;          /* PRNG seed for jitter */
} ShBackoff;

/* ============================================================================
 * Backoff API
 * ============================================================================ */

/*
 * Initialize a backoff iterator.
 *
 * @param backoff Backoff state to initialize
 * @param config  Configuration (NULL for defaults)
 */
void sh_backoff_init(ShBackoff *backoff, const ShBackoffConfig *config);

/*
 * Reset backoff iterator to initial state.
 * Keeps configuration, resets attempt counter.
 *
 * @param backoff Backoff state to reset
 */
void sh_backoff_reset(ShBackoff *backoff);

/*
 * Check if more retries are available.
 *
 * @param backoff Backoff state
 * @return 1 if attempt < max_retries, 0 otherwise
 */
int sh_backoff_has_retries(const ShBackoff *backoff);

/*
 * Get current attempt number.
 *
 * @param backoff Backoff state
 * @return Current attempt (0-indexed)
 */
int sh_backoff_attempt(const ShBackoff *backoff);

/*
 * Get next delay and advance attempt counter.
 * Returns the delay for the CURRENT attempt, then increments.
 *
 * Formula: min(base_delay * 2^attempt, max_delay) * (1 + jitter * random(-1, 1))
 *
 * @param backoff Backoff state
 * @return Delay in milliseconds for current attempt
 */
double sh_backoff_next(ShBackoff *backoff);

/*
 * Calculate delay for a specific attempt (stateless).
 * Does not require or modify any state.
 *
 * @param config  Configuration (NULL for defaults)
 * @param attempt Attempt number (0-indexed)
 * @return Delay in milliseconds
 */
double sh_backoff_calculate(const ShBackoffConfig *config, int attempt);

/*
 * Calculate delay with explicit seed for deterministic jitter (stateless).
 * Useful for testing or reproducible behavior.
 *
 * @param config  Configuration (NULL for defaults)
 * @param attempt Attempt number (0-indexed)
 * @param seed    Seed for jitter calculation
 * @return Delay in milliseconds
 */
double sh_backoff_calculate_seeded(const ShBackoffConfig *config, int attempt, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* SH_BACKOFF_H */
