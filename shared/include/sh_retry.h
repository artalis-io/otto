/*
 * sh_retry.h - HTTP Retry Logic
 *
 * Combines circuit breaker and exponential backoff for HTTP retry logic.
 * Handles retryable status codes (429, 500-504) and Retry-After headers.
 *
 * Usage:
 *   ShCircuitBreaker *cb = sh_circuit_create(NULL);
 *   ShRetryConfig config = SH_RETRY_DEFAULT_CONFIG;
 *   ShRetryContext ctx;
 *
 *   sh_retry_init(&ctx, cb, &config);
 *
 *   while (sh_retry_should_attempt(&ctx)) {
 *       int status = make_http_request(...);
 *       double delay = sh_retry_after_response(&ctx, status, retry_after_sec);
 *
 *       if (!sh_retry_should_continue(&ctx)) {
 *           break;  // Success or non-retryable error
 *       }
 *
 *       usleep((useconds_t)(delay * 1000));  // Wait before retry
 *   }
 *
 *   sh_circuit_free(cb);
 */

#ifndef SH_RETRY_H
#define SH_RETRY_H

#include "sh_backoff.h"
#include "sh_circuit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

/*
 * Retry configuration.
 */
typedef struct {
    ShBackoffConfig backoff;        /* Backoff settings */
    double retry_after_max_ms;      /* Max Retry-After to honor (default: 60000) */
    int retry_on_network_error;     /* Retry on network/connection errors (default: 1) */
    int retry_on_timeout;           /* Retry on timeout (default: 1) */
} ShRetryConfig;

/*
 * Default configuration: 3 retries, 100ms base, 60s max Retry-After
 */
#define SH_RETRY_DEFAULT_CONFIG { \
    .backoff = { \
        .base_delay_ms = 100.0, \
        .max_delay_ms = 10000.0, \
        .max_retries = 3, \
        .jitter_factor = 0.5 \
    }, \
    .retry_after_max_ms = 60000.0, \
    .retry_on_network_error = 1, \
    .retry_on_timeout = 1 \
}

/*
 * Retry context - tracks state across attempts.
 */
typedef struct {
    ShRetryConfig config;
    ShBackoff backoff;
    ShCircuitBreaker *circuit;  /* Optional - may be NULL */
    int last_status;            /* Last HTTP status code */
    int should_retry;           /* 1 if should retry, 0 if done */
    double delay_ms;            /* Computed delay for next retry */
} ShRetryContext;

/* ============================================================================
 * Retry API
 * ============================================================================ */

/*
 * Initialize a retry context.
 *
 * @param ctx     Retry context to initialize
 * @param circuit Optional circuit breaker (may be NULL)
 * @param config  Configuration (NULL for defaults)
 */
void sh_retry_init(ShRetryContext *ctx, ShCircuitBreaker *circuit, const ShRetryConfig *config);

/*
 * Reset retry context for a new request sequence.
 *
 * @param ctx Retry context
 */
void sh_retry_reset(ShRetryContext *ctx);

/*
 * Check if an attempt should be made.
 * Returns false if circuit is open or max retries exceeded.
 *
 * @param ctx Retry context
 * @return 1 if should attempt, 0 if should give up
 */
int sh_retry_should_attempt(ShRetryContext *ctx);

/*
 * Process response and compute retry delay.
 *
 * Call after each HTTP request completes (successfully or not).
 * Updates internal state and computes delay for next retry.
 *
 * @param ctx             Retry context
 * @param http_status     HTTP status code (0 for network error, -1 for timeout)
 * @param retry_after_sec Retry-After header value in seconds (0 if not present)
 * @return Delay in milliseconds before next retry (0 if no retry needed)
 */
double sh_retry_after_response(ShRetryContext *ctx, int http_status, double retry_after_sec);

/*
 * Check if retry should continue after processing response.
 *
 * @param ctx Retry context
 * @return 1 if should retry, 0 if done (success or fatal error)
 */
int sh_retry_should_continue(const ShRetryContext *ctx);

/*
 * Get the last recorded delay.
 *
 * @param ctx Retry context
 * @return Last computed delay in milliseconds
 */
double sh_retry_get_delay(const ShRetryContext *ctx);

/*
 * Get the last HTTP status code.
 *
 * @param ctx Retry context
 * @return Last HTTP status (0 for network error, -1 for timeout)
 */
int sh_retry_get_last_status(const ShRetryContext *ctx);

/*
 * Check if an HTTP status code is retryable.
 *
 * Retryable: 429, 500, 502, 503, 504
 * Not retryable: 4xx (except 429), other 5xx
 *
 * @param http_status HTTP status code
 * @return 1 if retryable, 0 if not
 */
int sh_retry_is_retryable_status(int http_status);

#ifdef __cplusplus
}
#endif

#endif /* SH_RETRY_H */
