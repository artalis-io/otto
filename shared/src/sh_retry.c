/*
 * sh_retry.c - HTTP Retry Logic Implementation
 */

#include "../include/sh_retry.h"
#include <string.h>

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_retry_init(ShRetryContext *ctx, ShCircuitBreaker *circuit, const ShRetryConfig *config)
{
    if (!ctx) return;

    memset(ctx, 0, sizeof(*ctx));

    if (config) {
        ctx->config = *config;
    } else {
        /* Apply defaults */
        ctx->config.backoff.base_delay_ms = 100.0;
        ctx->config.backoff.max_delay_ms = 10000.0;
        ctx->config.backoff.max_retries = 3;
        ctx->config.backoff.jitter_factor = 0.5;
        ctx->config.retry_after_max_ms = 60000.0;
        ctx->config.retry_on_network_error = 1;
        ctx->config.retry_on_timeout = 1;
    }

    sh_backoff_init(&ctx->backoff, &ctx->config.backoff);
    ctx->circuit = circuit;
    ctx->last_status = 0;
    ctx->should_retry = 0;
    ctx->delay_ms = 0;
}

void sh_retry_reset(ShRetryContext *ctx)
{
    if (!ctx) return;

    sh_backoff_reset(&ctx->backoff);
    ctx->last_status = 0;
    ctx->should_retry = 0;
    ctx->delay_ms = 0;
}

int sh_retry_should_attempt(ShRetryContext *ctx)
{
    if (!ctx) return 0;

    /* Check circuit breaker if present */
    if (ctx->circuit && !sh_circuit_allow(ctx->circuit)) {
        return 0;  /* Circuit is open */
    }

    /* Check if retries remain */
    return sh_backoff_has_retries(&ctx->backoff);
}

double sh_retry_after_response(ShRetryContext *ctx, int http_status, double retry_after_sec)
{
    if (!ctx) return 0;

    ctx->last_status = http_status;
    ctx->delay_ms = 0;
    ctx->should_retry = 0;

    /* Determine if this is a success */
    int is_success = (http_status >= 200 && http_status < 300);

    /* Record result with circuit breaker if present */
    if (ctx->circuit) {
        sh_circuit_record(ctx->circuit, is_success);
    }

    /* Success - no retry needed */
    if (is_success) {
        return 0;
    }

    /* Check if error is retryable */
    int is_retryable = 0;

    if (http_status == 0 && ctx->config.retry_on_network_error) {
        /* Network error */
        is_retryable = 1;
    } else if (http_status == -1 && ctx->config.retry_on_timeout) {
        /* Timeout */
        is_retryable = 1;
    } else if (sh_retry_is_retryable_status(http_status)) {
        is_retryable = 1;
    }

    if (!is_retryable) {
        /* Non-retryable error (e.g., 400, 401, 403, 404) */
        return 0;
    }

    /* Check if retries remain */
    if (!sh_backoff_has_retries(&ctx->backoff)) {
        /* No more retries available */
        return 0;
    }

    /* Calculate delay and consume one attempt */
    double delay = sh_backoff_next(&ctx->backoff);

    /* Honor Retry-After header if present and reasonable */
    if (retry_after_sec > 0) {
        double retry_after_ms = retry_after_sec * 1000.0;
        if (retry_after_ms <= ctx->config.retry_after_max_ms) {
            /* Use Retry-After instead of exponential backoff */
            delay = retry_after_ms;
        }
        /* If Retry-After exceeds max, use exponential backoff instead */
    }

    ctx->delay_ms = delay;

    /* Only set should_retry if there are attempts remaining for future retries */
    ctx->should_retry = sh_backoff_has_retries(&ctx->backoff);

    return delay;
}

int sh_retry_should_continue(const ShRetryContext *ctx)
{
    if (!ctx) return 0;
    return ctx->should_retry;
}

double sh_retry_get_delay(const ShRetryContext *ctx)
{
    if (!ctx) return 0;
    return ctx->delay_ms;
}

int sh_retry_get_last_status(const ShRetryContext *ctx)
{
    if (!ctx) return 0;
    return ctx->last_status;
}

int sh_retry_is_retryable_status(int http_status)
{
    /* 429 Too Many Requests */
    if (http_status == 429) return 1;

    /* 500 Internal Server Error */
    if (http_status == 500) return 1;

    /* 502 Bad Gateway */
    if (http_status == 502) return 1;

    /* 503 Service Unavailable */
    if (http_status == 503) return 1;

    /* 504 Gateway Timeout */
    if (http_status == 504) return 1;

    return 0;
}
