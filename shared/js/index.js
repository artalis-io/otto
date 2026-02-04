/**
 * Shared JavaScript Utilities for OTTO Platform
 *
 * API client resilience infrastructure:
 * - Circuit breaker pattern
 * - Exponential backoff with jitter
 * - Resilient fetch with retry logic
 */

export { CircuitBreaker, CircuitState } from './circuit-breaker.js';
export { createBackoff, calculateBackoff } from './backoff.js';
export {
    createResilientFetch,
    ResilientFetchError,
    isRetryableStatus
} from './resilient-fetch.js';
