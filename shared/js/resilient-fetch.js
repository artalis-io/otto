/**
 * Resilient Fetch - HTTP client with retry and circuit breaker
 *
 * Combines circuit breaker and exponential backoff for resilient HTTP requests.
 * Handles retryable status codes (429, 500-504) and Retry-After headers.
 *
 * Usage:
 *   const client = createResilientFetch({
 *       circuit: new CircuitBreaker({ failureThreshold: 5 }),
 *       maxRetries: 3
 *   });
 *
 *   try {
 *       const response = await client.fetch('/api/data');
 *       const data = await response.json();
 *   } catch (err) {
 *       if (err.isCircuitOpen) {
 *           console.log('Service unavailable - circuit breaker is open');
 *       }
 *   }
 */

import { CircuitBreaker } from './circuit-breaker.js';
import { createBackoff } from './backoff.js';

/**
 * Error class for resilient fetch failures.
 */
export class ResilientFetchError extends Error {
    /**
     * @param {string} message Error message
     * @param {Object} options
     * @param {number} options.status HTTP status code
     * @param {number} options.retryAfter Retry-After header value in seconds
     * @param {boolean} options.isCircuitOpen Whether circuit breaker is open
     * @param {boolean} options.isTimeout Whether request timed out
     * @param {Error} options.cause Original error
     */
    constructor(message, { status, retryAfter, isCircuitOpen, isTimeout, cause } = {}) {
        super(message);
        this.name = 'ResilientFetchError';
        this.status = status;
        this.retryAfter = retryAfter;
        this.isCircuitOpen = isCircuitOpen;
        this.isTimeout = isTimeout;
        this.cause = cause;
    }
}

/**
 * Check if an HTTP status code is retryable.
 *
 * @param {number} status HTTP status code
 * @returns {boolean}
 */
export function isRetryableStatus(status) {
    return status === 429 || status === 500 || status === 502 ||
           status === 503 || status === 504;
}

/**
 * Parse Retry-After header.
 *
 * @param {string|null} value Header value
 * @returns {number} Seconds to wait, or 0 if not parseable
 */
function parseRetryAfter(value) {
    if (!value) return 0;

    // Try parsing as seconds
    const seconds = parseInt(value, 10);
    if (!isNaN(seconds) && seconds > 0) {
        return seconds;
    }

    // Try parsing as HTTP date
    const date = Date.parse(value);
    if (!isNaN(date)) {
        const delayMs = date - Date.now();
        return Math.max(0, Math.ceil(delayMs / 1000));
    }

    return 0;
}

/**
 * Create a resilient fetch client.
 *
 * @param {Object} options Configuration options
 * @param {CircuitBreaker} options.circuit Circuit breaker instance
 * @param {number} options.timeoutMs Request timeout (default: 30000)
 * @param {number} options.maxRetries Maximum retry attempts (default: 3)
 * @param {number} options.baseDelayMs Initial retry delay (default: 100)
 * @param {number} options.maxDelayMs Maximum retry delay (default: 10000)
 * @param {number} options.retryAfterMaxMs Maximum Retry-After to honor (default: 60000)
 * @param {boolean} options.retryOnNetworkError Retry on network errors (default: true)
 * @param {boolean} options.retryOnTimeout Retry on timeout (default: true)
 * @returns {Object} Resilient fetch client
 */
export function createResilientFetch({
    circuit = null,
    timeoutMs = 30000,
    maxRetries = 3,
    baseDelayMs = 100,
    maxDelayMs = 10000,
    retryAfterMaxMs = 60000,
    retryOnNetworkError = true,
    retryOnTimeout = true
} = {}) {
    // Create circuit breaker if not provided
    const _circuit = circuit || new CircuitBreaker();

    return {
        /**
         * Get the circuit breaker instance.
         */
        get circuit() {
            return _circuit;
        },

        /**
         * Make a resilient fetch request.
         *
         * @param {string} url Request URL
         * @param {Object} options Fetch options
         * @returns {Promise<Response>} Fetch response
         * @throws {ResilientFetchError} On failure after all retries
         */
        async fetch(url, options = {}) {
            const backoff = createBackoff({ baseDelayMs, maxDelayMs, maxRetries });
            let lastError = null;
            let lastStatus = 0;

            while (backoff.hasRetries()) {
                // Check circuit breaker
                if (!_circuit.allow()) {
                    throw new ResilientFetchError(
                        'Circuit breaker is open',
                        { isCircuitOpen: true }
                    );
                }

                try {
                    // Create abort controller for timeout
                    const controller = new AbortController();
                    const timeoutId = setTimeout(() => controller.abort(), timeoutMs);

                    try {
                        const response = await fetch(url, {
                            ...options,
                            signal: controller.signal
                        });

                        clearTimeout(timeoutId);
                        lastStatus = response.status;

                        // Success - record and return
                        if (response.ok) {
                            _circuit.success();
                            return response;
                        }

                        // Check if status is retryable
                        if (!isRetryableStatus(response.status)) {
                            // Non-retryable error - record failure and throw
                            _circuit.failure();
                            throw new ResilientFetchError(
                                `HTTP ${response.status}`,
                                { status: response.status }
                            );
                        }

                        // Retryable status - record failure
                        _circuit.failure();

                        // Check for Retry-After header
                        const retryAfterSec = parseRetryAfter(
                            response.headers.get('Retry-After')
                        );

                        // Check if we have retries left
                        if (!backoff.hasRetries()) {
                            throw new ResilientFetchError(
                                `HTTP ${response.status} after ${maxRetries} retries`,
                                { status: response.status, retryAfter: retryAfterSec }
                            );
                        }

                        // Calculate delay
                        let delay = backoff.next();

                        // Honor Retry-After if reasonable
                        if (retryAfterSec > 0) {
                            const retryAfterMs = retryAfterSec * 1000;
                            if (retryAfterMs <= retryAfterMaxMs) {
                                delay = retryAfterMs;
                            }
                        }

                        // Wait before retry
                        if (delay > 0) {
                            await new Promise(resolve => setTimeout(resolve, delay));
                        }

                        lastError = new ResilientFetchError(
                            `HTTP ${response.status}`,
                            { status: response.status }
                        );

                    } catch (fetchErr) {
                        clearTimeout(timeoutId);

                        // Check for abort/timeout
                        if (fetchErr.name === 'AbortError') {
                            _circuit.failure();

                            if (!retryOnTimeout || !backoff.hasRetries()) {
                                throw new ResilientFetchError(
                                    'Request timeout',
                                    { isTimeout: true, cause: fetchErr }
                                );
                            }

                            // Wait and retry
                            const delay = backoff.next();
                            if (delay > 0) {
                                await new Promise(resolve => setTimeout(resolve, delay));
                            }

                            lastError = new ResilientFetchError(
                                'Request timeout',
                                { isTimeout: true, cause: fetchErr }
                            );
                            continue;
                        }

                        throw fetchErr;
                    }

                } catch (err) {
                    // Network error
                    if (err instanceof ResilientFetchError) {
                        throw err;
                    }

                    _circuit.failure();

                    if (!retryOnNetworkError || !backoff.hasRetries()) {
                        throw new ResilientFetchError(
                            `Network error: ${err.message}`,
                            { cause: err }
                        );
                    }

                    // Wait and retry
                    const delay = backoff.next();
                    if (delay > 0) {
                        await new Promise(resolve => setTimeout(resolve, delay));
                    }

                    lastError = new ResilientFetchError(
                        `Network error: ${err.message}`,
                        { cause: err }
                    );
                }
            }

            // Exhausted retries
            throw lastError || new ResilientFetchError(
                `Failed after ${maxRetries} retries`,
                { status: lastStatus }
            );
        }
    };
}
