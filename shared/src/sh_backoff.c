/*
 * sh_backoff.c - Exponential Backoff with Jitter Implementation
 */

#include "../include/sh_backoff.h"
#include <sys/time.h>
#include <stddef.h>

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Simple xorshift64 PRNG for jitter.
 * Fast, decent quality, no external dependencies.
 */
static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/*
 * Generate random double in range [0, 1).
 */
static double random_unit(uint64_t *seed)
{
    return (double)(xorshift64(seed) >> 11) / (double)(1ULL << 53);
}

/*
 * Get entropy seed from system time.
 */
static uint64_t get_entropy_seed(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_backoff_init(ShBackoff *backoff, const ShBackoffConfig *config)
{
    if (!backoff) return;

    if (config) {
        backoff->config = *config;
    } else {
        backoff->config.base_delay_ms = 100.0;
        backoff->config.max_delay_ms = 10000.0;
        backoff->config.max_retries = 5;
        backoff->config.jitter_factor = 0.5;
    }

    /* Validate config */
    if (backoff->config.base_delay_ms < 0) backoff->config.base_delay_ms = 0;
    if (backoff->config.max_delay_ms < 0) backoff->config.max_delay_ms = 0;
    if (backoff->config.max_retries < 0) backoff->config.max_retries = 0;
    if (backoff->config.jitter_factor < 0) backoff->config.jitter_factor = 0;
    if (backoff->config.jitter_factor > 1.0) backoff->config.jitter_factor = 1.0;

    backoff->attempt = 0;
    backoff->last_delay_ms = 0;
    backoff->seed = get_entropy_seed();
}

void sh_backoff_reset(ShBackoff *backoff)
{
    if (!backoff) return;
    backoff->attempt = 0;
    backoff->last_delay_ms = 0;
    /* Keep seed for continuity */
}

int sh_backoff_has_retries(const ShBackoff *backoff)
{
    if (!backoff) return 0;
    return backoff->attempt < backoff->config.max_retries;
}

int sh_backoff_attempt(const ShBackoff *backoff)
{
    if (!backoff) return 0;
    return backoff->attempt;
}

double sh_backoff_next(ShBackoff *backoff)
{
    if (!backoff) return 0;

    double delay = sh_backoff_calculate_seeded(&backoff->config, backoff->attempt, backoff->seed);

    /* Advance PRNG state */
    xorshift64(&backoff->seed);

    backoff->last_delay_ms = delay;
    backoff->attempt++;

    return delay;
}

double sh_backoff_calculate(const ShBackoffConfig *config, int attempt)
{
    return sh_backoff_calculate_seeded(config, attempt, get_entropy_seed());
}

double sh_backoff_calculate_seeded(const ShBackoffConfig *config, int attempt, uint64_t seed)
{
    /* Use defaults if no config provided */
    double base_delay = config ? config->base_delay_ms : 100.0;
    double max_delay = config ? config->max_delay_ms : 10000.0;
    double jitter_factor = config ? config->jitter_factor : 0.5;

    if (attempt < 0) attempt = 0;

    /* Exponential backoff: base * 2^attempt */
    double delay = base_delay;
    for (int i = 0; i < attempt && delay < max_delay; i++) {
        delay *= 2.0;
    }

    /* Cap at max delay */
    if (delay > max_delay) {
        delay = max_delay;
    }

    /* Apply jitter: multiply by (1 + jitter_factor * random(-1, 1)) */
    if (jitter_factor > 0) {
        uint64_t local_seed = seed;
        double rand_val = random_unit(&local_seed);  /* [0, 1) */
        double jitter = (rand_val * 2.0 - 1.0) * jitter_factor;  /* [-jitter_factor, +jitter_factor) */
        delay *= (1.0 + jitter);

        /* Ensure delay stays positive */
        if (delay < 0) delay = 0;
    }

    return delay;
}
