/**
 * Exponential Backoff with Jitter
 *
 * Provides exponential backoff calculations for retry logic.
 * Supports both stateful iterator and stateless calculation APIs.
 *
 * Usage (stateful):
 *   const backoff = createBackoff({ maxRetries: 5 });
 *   while (backoff.hasRetries()) {
 *       await backoff.wait();
 *       if (await tryRequest()) break;
 *   }
 *
 * Usage (stateless):
 *   const delay = calculateBackoff(attempt, { baseDelayMs: 100 });
 */

/**
 * Calculate backoff delay for a specific attempt.
 *
 * @param {number} attempt Attempt number (0-indexed)
 * @param {Object} options Configuration options
 * @param {number} options.baseDelayMs Initial delay (default: 100)
 * @param {number} options.maxDelayMs Maximum delay cap (default: 10000)
 * @param {number} options.jitterFactor Jitter multiplier 0-1 (default: 0.5)
 * @returns {number} Delay in milliseconds
 */
export function calculateBackoff(attempt, {
    baseDelayMs = 100,
    maxDelayMs = 10000,
    jitterFactor = 0.5
} = {}) {
    if (attempt < 0) attempt = 0;

    // Exponential backoff: base * 2^attempt
    let delay = baseDelayMs * Math.pow(2, attempt);

    // Cap at max delay
    if (delay > maxDelayMs) {
        delay = maxDelayMs;
    }

    // Apply jitter: multiply by (1 + jitterFactor * random(-1, 1))
    if (jitterFactor > 0) {
        const jitter = (Math.random() * 2 - 1) * jitterFactor;
        delay *= (1 + jitter);
    }

    return Math.max(0, delay);
}

/**
 * Create a stateful backoff iterator.
 *
 * @param {Object} options Configuration options
 * @param {number} options.baseDelayMs Initial delay (default: 100)
 * @param {number} options.maxDelayMs Maximum delay cap (default: 10000)
 * @param {number} options.maxRetries Maximum retry attempts (default: 5)
 * @param {number} options.jitterFactor Jitter multiplier 0-1 (default: 0.5)
 * @returns {Object} Backoff iterator
 */
export function createBackoff({
    baseDelayMs = 100,
    maxDelayMs = 10000,
    maxRetries = 5,
    jitterFactor = 0.5
} = {}) {
    let attempt = 0;
    let lastDelay = 0;

    const config = { baseDelayMs, maxDelayMs, jitterFactor };

    return {
        /**
         * Check if more retries are available.
         * @returns {boolean}
         */
        hasRetries() {
            return attempt < maxRetries;
        },

        /**
         * Get current attempt number.
         * @returns {number}
         */
        attempt() {
            return attempt;
        },

        /**
         * Get next delay and advance attempt counter.
         * @returns {number} Delay in milliseconds
         */
        next() {
            const delay = calculateBackoff(attempt, config);
            lastDelay = delay;
            attempt++;
            return delay;
        },

        /**
         * Wait for next delay (async).
         * @returns {Promise<void>}
         */
        async wait() {
            const delay = this.next();
            if (delay > 0) {
                await new Promise(resolve => setTimeout(resolve, delay));
            }
        },

        /**
         * Get last computed delay.
         * @returns {number}
         */
        lastDelay() {
            return lastDelay;
        },

        /**
         * Reset backoff iterator.
         */
        reset() {
            attempt = 0;
            lastDelay = 0;
        }
    };
}
