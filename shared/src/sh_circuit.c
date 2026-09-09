/*
 * sh_circuit.c - Circuit Breaker Pattern Implementation
 */

#include "../include/sh_circuit.h"
#include "sh_pal.h"
#include <stdlib.h>
#include <string.h>
#include "sh_pal.h"

/* ============================================================================
 * Internal Types
 * ============================================================================ */

struct ShCircuitBreaker {
    ShCircuitConfig config;
    ShCircuitState state;
    int failure_count;          /* Consecutive failures (CLOSED) */
    int success_count;          /* Consecutive successes (HALF_OPEN) */
    double opened_at;           /* Timestamp when circuit opened */
    ShMutex mutex;

    /* Statistics */
    uint64_t total_requests;
    uint64_t total_failures;
    uint64_t total_blocked;
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static double get_time_ms(void)
{
    return (double)sh_wall_ns() / 1.0e6;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

ShCircuitBreaker *sh_circuit_create(const ShCircuitConfig *config)
{
    ShCircuitBreaker *cb = calloc(1, sizeof(ShCircuitBreaker));
    if (!cb) return NULL;

    /* Apply config or defaults */
    if (config) {
        cb->config = *config;
    } else {
        cb->config.failure_threshold = 5;
        cb->config.success_threshold = 2;
        cb->config.open_duration_ms = 30000.0;
    }

    /* Validate config */
    if (cb->config.failure_threshold < 1) cb->config.failure_threshold = 1;
    if (cb->config.success_threshold < 1) cb->config.success_threshold = 1;
    if (cb->config.open_duration_ms < 0) cb->config.open_duration_ms = 0;

    cb->state = SH_CIRCUIT_CLOSED;

    if (sh_mutex_init(&cb->mutex) != 0) {
        free(cb);
        return NULL;
    }

    return cb;
}

void sh_circuit_free(ShCircuitBreaker *cb)
{
    if (!cb) return;
    sh_mutex_destroy(&cb->mutex);
    free(cb);
}

int sh_circuit_allow(ShCircuitBreaker *cb)
{
    if (!cb) return 0;

    int allowed = 0;

    sh_mutex_lock(&cb->mutex);

    cb->total_requests++;

    switch (cb->state) {
        case SH_CIRCUIT_CLOSED:
            allowed = 1;
            break;

        case SH_CIRCUIT_OPEN: {
            double now = get_time_ms();
            double elapsed = now - cb->opened_at;
            if (elapsed >= cb->config.open_duration_ms) {
                /* Transition to half-open */
                cb->state = SH_CIRCUIT_HALF_OPEN;
                cb->success_count = 0;
                allowed = 1;
            } else {
                cb->total_blocked++;
                allowed = 0;
            }
            break;
        }

        case SH_CIRCUIT_HALF_OPEN:
            /* Allow request to test if service recovered */
            allowed = 1;
            break;
    }

    sh_mutex_unlock(&cb->mutex);
    return allowed;
}

void sh_circuit_record(ShCircuitBreaker *cb, int success)
{
    if (!cb) return;

    sh_mutex_lock(&cb->mutex);

    if (!success) {
        cb->total_failures++;
    }

    switch (cb->state) {
        case SH_CIRCUIT_CLOSED:
            if (success) {
                /* Reset failure counter on success */
                cb->failure_count = 0;
            } else {
                cb->failure_count++;
                if (cb->failure_count >= cb->config.failure_threshold) {
                    /* Trip the circuit */
                    cb->state = SH_CIRCUIT_OPEN;
                    cb->opened_at = get_time_ms();
                    cb->failure_count = 0;
                }
            }
            break;

        case SH_CIRCUIT_HALF_OPEN:
            if (success) {
                cb->success_count++;
                if (cb->success_count >= cb->config.success_threshold) {
                    /* Service recovered - close circuit */
                    cb->state = SH_CIRCUIT_CLOSED;
                    cb->success_count = 0;
                    cb->failure_count = 0;
                }
            } else {
                /* Failure in half-open - back to open */
                cb->state = SH_CIRCUIT_OPEN;
                cb->opened_at = get_time_ms();
                cb->success_count = 0;
            }
            break;

        case SH_CIRCUIT_OPEN:
            /* Should not happen - requests blocked in open state */
            break;
    }

    sh_mutex_unlock(&cb->mutex);
}

ShCircuitState sh_circuit_state(ShCircuitBreaker *cb)
{
    if (!cb) return SH_CIRCUIT_OPEN;  /* Deny by default for invalid args */

    ShCircuitState state;
    sh_mutex_lock(&cb->mutex);
    state = cb->state;
    sh_mutex_unlock(&cb->mutex);
    return state;
}

void sh_circuit_stats(ShCircuitBreaker *cb, ShCircuitStats *stats)
{
    if (!cb || !stats) return;

    sh_mutex_lock(&cb->mutex);
    stats->total_requests = cb->total_requests;
    stats->total_failures = cb->total_failures;
    stats->total_blocked = cb->total_blocked;
    stats->state = cb->state;
    sh_mutex_unlock(&cb->mutex);
}

void sh_circuit_reset(ShCircuitBreaker *cb)
{
    if (!cb) return;

    sh_mutex_lock(&cb->mutex);
    cb->state = SH_CIRCUIT_CLOSED;
    cb->failure_count = 0;
    cb->success_count = 0;
    cb->opened_at = 0;
    /* Don't reset statistics - keep historical data */
    sh_mutex_unlock(&cb->mutex);
}
