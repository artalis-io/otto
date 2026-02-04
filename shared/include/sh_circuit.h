/*
 * sh_circuit.h - Circuit Breaker Pattern
 *
 * Thread-safe circuit breaker for protecting against cascading failures.
 * Implements the standard CLOSED -> OPEN -> HALF_OPEN -> CLOSED cycle.
 *
 * Usage:
 *   // Create circuit breaker with defaults
 *   ShCircuitConfig config = SH_CIRCUIT_DEFAULT_CONFIG;
 *   ShCircuitBreaker *cb = sh_circuit_create(&config);
 *
 *   // Before each request
 *   if (!sh_circuit_allow(cb)) {
 *       // Circuit is open - fail fast
 *       return ERROR_CIRCUIT_OPEN;
 *   }
 *
 *   // After request completes
 *   if (success) {
 *       sh_circuit_record(cb, 1);  // Success
 *   } else {
 *       sh_circuit_record(cb, 0);  // Failure
 *   }
 *
 *   // Cleanup
 *   sh_circuit_free(cb);
 */

#ifndef SH_CIRCUIT_H
#define SH_CIRCUIT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Circuit breaker states.
 *
 * CLOSED:    Normal operation, requests flow through
 * OPEN:      Failing fast, all requests rejected
 * HALF_OPEN: Testing if service recovered, limited requests allowed
 */
typedef enum {
    SH_CIRCUIT_CLOSED,
    SH_CIRCUIT_OPEN,
    SH_CIRCUIT_HALF_OPEN
} ShCircuitState;

/*
 * Circuit breaker configuration.
 */
typedef struct {
    int failure_threshold;      /* Consecutive failures before opening (default: 5) */
    int success_threshold;      /* Consecutive successes to close (default: 2) */
    double open_duration_ms;    /* Time to stay open before half-open (default: 30000) */
} ShCircuitConfig;

/*
 * Default configuration: 5 failures to open, 2 successes to close, 30s open duration
 */
#define SH_CIRCUIT_DEFAULT_CONFIG { \
    .failure_threshold = 5, \
    .success_threshold = 2, \
    .open_duration_ms = 30000.0 \
}

/*
 * Circuit breaker statistics.
 */
typedef struct {
    uint64_t total_requests;    /* Total requests (allowed + blocked) */
    uint64_t total_failures;    /* Total recorded failures */
    uint64_t total_blocked;     /* Total requests blocked when open */
    ShCircuitState state;       /* Current state */
} ShCircuitStats;

/*
 * Circuit breaker instance.
 * Opaque handle - use sh_circuit_create() to construct.
 */
typedef struct ShCircuitBreaker ShCircuitBreaker;

/* ============================================================================
 * Circuit Breaker API
 * ============================================================================ */

/*
 * Create a new circuit breaker.
 *
 * @param config Configuration (NULL for defaults)
 * @return New circuit breaker, or NULL on allocation failure
 *
 * OWNERSHIP: Caller owns the returned breaker and must call sh_circuit_free().
 */
ShCircuitBreaker *sh_circuit_create(const ShCircuitConfig *config);

/*
 * Free a circuit breaker.
 * Safe to call with NULL (no-op).
 *
 * @param cb Circuit breaker to free (may be NULL)
 */
void sh_circuit_free(ShCircuitBreaker *cb);

/*
 * Check if a request is allowed through the circuit.
 * Thread-safe - can be called from multiple threads concurrently.
 *
 * In CLOSED state: always allows
 * In OPEN state: blocks until open_duration_ms elapsed, then transitions to HALF_OPEN
 * In HALF_OPEN state: allows (to test if service recovered)
 *
 * @param cb Circuit breaker instance
 * @return 1 if request is allowed, 0 if blocked (circuit is open)
 */
int sh_circuit_allow(ShCircuitBreaker *cb);

/*
 * Record the result of a request.
 * Thread-safe - can be called from multiple threads concurrently.
 *
 * In CLOSED state:
 *   - Failures increment counter; threshold reached -> OPEN
 *   - Success resets failure counter
 *
 * In HALF_OPEN state:
 *   - Success increments counter; threshold reached -> CLOSED
 *   - Failure immediately returns to OPEN
 *
 * In OPEN state:
 *   - Recording is a no-op (requests should not reach backend)
 *
 * @param cb      Circuit breaker instance
 * @param success 1 if request succeeded, 0 if failed
 */
void sh_circuit_record(ShCircuitBreaker *cb, int success);

/*
 * Get current circuit state.
 * Thread-safe.
 *
 * @param cb Circuit breaker instance
 * @return Current state
 */
ShCircuitState sh_circuit_state(ShCircuitBreaker *cb);

/*
 * Get circuit breaker statistics.
 * Thread-safe.
 *
 * @param cb    Circuit breaker instance
 * @param stats Output statistics (may be NULL to skip)
 */
void sh_circuit_stats(ShCircuitBreaker *cb, ShCircuitStats *stats);

/*
 * Reset circuit breaker to initial CLOSED state.
 * Thread-safe.
 *
 * @param cb Circuit breaker instance
 */
void sh_circuit_reset(ShCircuitBreaker *cb);

#ifdef __cplusplus
}
#endif

#endif /* SH_CIRCUIT_H */
