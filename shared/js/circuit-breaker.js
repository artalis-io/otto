/**
 * Circuit Breaker Pattern
 *
 * Prevents cascading failures by failing fast when a service is unhealthy.
 * Implements CLOSED -> OPEN -> HALF_OPEN -> CLOSED cycle.
 *
 * Usage:
 *   const circuit = new CircuitBreaker({ failureThreshold: 5 });
 *
 *   if (!circuit.allow()) {
 *       throw new Error('Circuit is open');
 *   }
 *
 *   try {
 *       await makeRequest();
 *       circuit.success();
 *   } catch (err) {
 *       circuit.failure();
 *       throw err;
 *   }
 */

export const CircuitState = {
    CLOSED: 'closed',
    OPEN: 'open',
    HALF_OPEN: 'half_open'
};

export class CircuitBreaker {
    /**
     * Create a circuit breaker.
     *
     * @param {Object} options Configuration options
     * @param {number} options.failureThreshold Consecutive failures before opening (default: 5)
     * @param {number} options.successThreshold Consecutive successes to close (default: 2)
     * @param {number} options.openDurationMs Time to stay open before half-open (default: 30000)
     */
    constructor({
        failureThreshold = 5,
        successThreshold = 2,
        openDurationMs = 30000
    } = {}) {
        this._failureThreshold = failureThreshold;
        this._successThreshold = successThreshold;
        this._openDurationMs = openDurationMs;

        this._state = CircuitState.CLOSED;
        this._failureCount = 0;
        this._successCount = 0;
        this._openedAt = 0;

        // Statistics
        this._totalRequests = 0;
        this._totalFailures = 0;
        this._totalBlocked = 0;
    }

    /**
     * Check if a request is allowed through the circuit.
     *
     * @returns {boolean} True if request is allowed, false if blocked
     */
    allow() {
        this._totalRequests++;

        switch (this._state) {
            case CircuitState.CLOSED:
                return true;

            case CircuitState.OPEN: {
                const elapsed = Date.now() - this._openedAt;
                if (elapsed >= this._openDurationMs) {
                    // Transition to half-open
                    this._state = CircuitState.HALF_OPEN;
                    this._successCount = 0;
                    return true;
                }
                this._totalBlocked++;
                return false;
            }

            case CircuitState.HALF_OPEN:
                // Allow request to test if service recovered
                return true;

            default:
                return false;
        }
    }

    /**
     * Record a successful request.
     */
    success() {
        switch (this._state) {
            case CircuitState.CLOSED:
                // Reset failure counter on success
                this._failureCount = 0;
                break;

            case CircuitState.HALF_OPEN:
                this._successCount++;
                if (this._successCount >= this._successThreshold) {
                    // Service recovered - close circuit
                    this._state = CircuitState.CLOSED;
                    this._successCount = 0;
                    this._failureCount = 0;
                }
                break;

            case CircuitState.OPEN:
                // Should not happen - requests blocked in open state
                break;
        }
    }

    /**
     * Record a failed request.
     */
    failure() {
        this._totalFailures++;

        switch (this._state) {
            case CircuitState.CLOSED:
                this._failureCount++;
                if (this._failureCount >= this._failureThreshold) {
                    // Trip the circuit
                    this._state = CircuitState.OPEN;
                    this._openedAt = Date.now();
                    this._failureCount = 0;
                }
                break;

            case CircuitState.HALF_OPEN:
                // Failure in half-open - back to open
                this._state = CircuitState.OPEN;
                this._openedAt = Date.now();
                this._successCount = 0;
                break;

            case CircuitState.OPEN:
                // Should not happen
                break;
        }
    }

    /**
     * Reset circuit breaker to initial CLOSED state.
     */
    reset() {
        this._state = CircuitState.CLOSED;
        this._failureCount = 0;
        this._successCount = 0;
        this._openedAt = 0;
    }

    /**
     * Get current circuit state.
     *
     * @returns {string} Current state
     */
    getState() {
        return this._state;
    }

    /**
     * Get circuit breaker statistics.
     *
     * @returns {Object} Statistics object
     */
    getStats() {
        return {
            totalRequests: this._totalRequests,
            totalFailures: this._totalFailures,
            totalBlocked: this._totalBlocked,
            state: this._state
        };
    }
}
