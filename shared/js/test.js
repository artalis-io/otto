/**
 * Tests for shared/js resilience modules
 *
 * Run: node test.js
 */

import { CircuitBreaker, CircuitState } from './circuit-breaker.js';
import { createBackoff, calculateBackoff } from './backoff.js';
import { createResilientFetch, ResilientFetchError, isRetryableStatus } from './resilient-fetch.js';

// Simple test framework
let testsRun = 0;
let testsPassed = 0;

function test(name, fn) {
    testsRun++;
    try {
        fn();
        testsPassed++;
        console.log(`  [PASS] ${name}`);
    } catch (err) {
        console.log(`  [FAIL] ${name}`);
        console.log(`    ${err.message}`);
    }
}

async function testAsync(name, fn) {
    testsRun++;
    try {
        await fn();
        testsPassed++;
        console.log(`  [PASS] ${name}`);
    } catch (err) {
        console.log(`  [FAIL] ${name}`);
        console.log(`    ${err.message}`);
    }
}

function assert(condition, message = 'Assertion failed') {
    if (!condition) throw new Error(message);
}

function assertEqual(actual, expected, message) {
    if (actual !== expected) {
        throw new Error(message || `Expected ${expected}, got ${actual}`);
    }
}

function assertNear(actual, expected, tolerance, message) {
    if (Math.abs(actual - expected) > tolerance) {
        throw new Error(message || `Expected ${expected} ± ${tolerance}, got ${actual}`);
    }
}

// =============================================================================
// Circuit Breaker Tests
// =============================================================================

console.log('\nCircuit Breaker:');

test('create with defaults', () => {
    const cb = new CircuitBreaker();
    assertEqual(cb.getState(), CircuitState.CLOSED);
});

test('create with config', () => {
    const cb = new CircuitBreaker({
        failureThreshold: 3,
        successThreshold: 1,
        openDurationMs: 5000
    });
    assertEqual(cb.getState(), CircuitState.CLOSED);
});

test('allow when closed', () => {
    const cb = new CircuitBreaker();
    for (let i = 0; i < 10; i++) {
        assertEqual(cb.allow(), true);
    }
});

test('opens on failures', () => {
    const cb = new CircuitBreaker({ failureThreshold: 3 });

    cb.allow();
    cb.failure();
    assertEqual(cb.getState(), CircuitState.CLOSED);

    cb.allow();
    cb.failure();
    assertEqual(cb.getState(), CircuitState.CLOSED);

    cb.allow();
    cb.failure();
    assertEqual(cb.getState(), CircuitState.OPEN);
});

test('blocks when open', () => {
    const cb = new CircuitBreaker({ failureThreshold: 2, openDurationMs: 100000 });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    assertEqual(cb.getState(), CircuitState.OPEN);

    assertEqual(cb.allow(), false);
});

test('success resets failure count', () => {
    const cb = new CircuitBreaker({ failureThreshold: 3 });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    cb.allow(); cb.success();  // Should reset

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    assertEqual(cb.getState(), CircuitState.CLOSED);  // Not open yet
});

test('half-open after duration', async () => {
    const cb = new CircuitBreaker({ failureThreshold: 2, openDurationMs: 10 });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    assertEqual(cb.getState(), CircuitState.OPEN);

    await new Promise(r => setTimeout(r, 20));
    assertEqual(cb.allow(), true);
    assertEqual(cb.getState(), CircuitState.HALF_OPEN);
});

test('half-open closes on success', async () => {
    const cb = new CircuitBreaker({
        failureThreshold: 2,
        successThreshold: 2,
        openDurationMs: 10
    });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    await new Promise(r => setTimeout(r, 20));
    cb.allow();  // Transitions to half-open

    cb.success();
    assertEqual(cb.getState(), CircuitState.HALF_OPEN);

    cb.success();
    assertEqual(cb.getState(), CircuitState.CLOSED);
});

test('half-open reopens on failure', async () => {
    const cb = new CircuitBreaker({ failureThreshold: 2, openDurationMs: 10 });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    await new Promise(r => setTimeout(r, 20));
    cb.allow();  // Transitions to half-open
    assertEqual(cb.getState(), CircuitState.HALF_OPEN);

    cb.failure();
    assertEqual(cb.getState(), CircuitState.OPEN);
});

test('reset returns to closed', () => {
    const cb = new CircuitBreaker({ failureThreshold: 2, openDurationMs: 100000 });

    cb.allow(); cb.failure();
    cb.allow(); cb.failure();
    assertEqual(cb.getState(), CircuitState.OPEN);

    cb.reset();
    assertEqual(cb.getState(), CircuitState.CLOSED);
});

test('stats tracking', () => {
    const cb = new CircuitBreaker({ failureThreshold: 3 });

    cb.allow();
    cb.allow();
    cb.failure();
    cb.success();

    const stats = cb.getStats();
    assertEqual(stats.totalRequests, 2);
    assertEqual(stats.totalFailures, 1);
    assertEqual(stats.state, CircuitState.CLOSED);
});

// =============================================================================
// Backoff Tests
// =============================================================================

console.log('\nExponential Backoff:');

test('createBackoff with defaults', () => {
    const backoff = createBackoff();
    assertEqual(backoff.attempt(), 0);
    assertEqual(backoff.hasRetries(), true);
});

test('createBackoff with config', () => {
    const backoff = createBackoff({ maxRetries: 3 });
    assertEqual(backoff.hasRetries(), true);
});

test('exponential growth', () => {
    const backoff = createBackoff({
        baseDelayMs: 100,
        maxDelayMs: 10000,
        maxRetries: 5,
        jitterFactor: 0  // No jitter for predictable test
    });

    assertNear(backoff.next(), 100, 1);
    assertNear(backoff.next(), 200, 1);
    assertNear(backoff.next(), 400, 1);
    assertNear(backoff.next(), 800, 1);
    assertNear(backoff.next(), 1600, 1);
});

test('max delay cap', () => {
    const backoff = createBackoff({
        baseDelayMs: 1000,
        maxDelayMs: 2000,
        maxRetries: 5,
        jitterFactor: 0
    });

    backoff.next();  // 1000
    assertNear(backoff.next(), 2000, 1);  // Capped
    assertNear(backoff.next(), 2000, 1);  // Still capped
});

test('hasRetries respects maxRetries', () => {
    const backoff = createBackoff({ maxRetries: 2 });

    assertEqual(backoff.hasRetries(), true);
    backoff.next();
    assertEqual(backoff.hasRetries(), true);
    backoff.next();
    assertEqual(backoff.hasRetries(), false);
});

test('reset clears attempt counter', () => {
    const backoff = createBackoff({ maxRetries: 5, jitterFactor: 0, baseDelayMs: 100 });

    backoff.next();
    backoff.next();
    assertEqual(backoff.attempt(), 2);

    backoff.reset();
    assertEqual(backoff.attempt(), 0);
    assertNear(backoff.next(), 100, 1);
});

test('calculateBackoff stateless', () => {
    const config = { baseDelayMs: 100, maxDelayMs: 10000, jitterFactor: 0 };

    assertNear(calculateBackoff(0, config), 100, 1);
    assertNear(calculateBackoff(1, config), 200, 1);
    assertNear(calculateBackoff(2, config), 400, 1);
});

test('jitter stays in range', () => {
    const config = { baseDelayMs: 1000, maxDelayMs: 10000, jitterFactor: 0.5 };

    for (let i = 0; i < 20; i++) {
        const delay = calculateBackoff(0, config);
        assert(delay >= 500 && delay <= 1500, `Delay ${delay} out of range`);
    }
});

// =============================================================================
// Resilient Fetch Tests
// =============================================================================

console.log('\nResilient Fetch:');

test('isRetryableStatus', () => {
    assertEqual(isRetryableStatus(429), true);
    assertEqual(isRetryableStatus(500), true);
    assertEqual(isRetryableStatus(502), true);
    assertEqual(isRetryableStatus(503), true);
    assertEqual(isRetryableStatus(504), true);
    assertEqual(isRetryableStatus(400), false);
    assertEqual(isRetryableStatus(401), false);
    assertEqual(isRetryableStatus(404), false);
    assertEqual(isRetryableStatus(200), false);
});

test('ResilientFetchError properties', () => {
    const err = new ResilientFetchError('Test error', {
        status: 503,
        isCircuitOpen: false,
        isTimeout: true
    });

    assertEqual(err.name, 'ResilientFetchError');
    assertEqual(err.status, 503);
    assertEqual(err.isCircuitOpen, false);
    assertEqual(err.isTimeout, true);
});

test('createResilientFetch returns client', () => {
    const client = createResilientFetch();
    assert(typeof client.fetch === 'function');
    assert(client.circuit instanceof CircuitBreaker);
});

test('createResilientFetch with custom circuit', () => {
    const circuit = new CircuitBreaker({ failureThreshold: 10 });
    const client = createResilientFetch({ circuit });
    assertEqual(client.circuit, circuit);
});

// Mock fetch for testing (requires global fetch to exist)
if (typeof fetch === 'undefined') {
    console.log('\n  [SKIP] fetch-based tests (fetch not available in Node without polyfill)');
} else {
    await testAsync('blocks when circuit open', async () => {
        const circuit = new CircuitBreaker({ failureThreshold: 1 });
        const client = createResilientFetch({ circuit });

        // Trip the circuit
        circuit.failure();
        circuit.failure();

        try {
            await client.fetch('http://example.com');
            assert(false, 'Should have thrown');
        } catch (err) {
            assertEqual(err.isCircuitOpen, true);
        }
    });
}

// =============================================================================
// Summary
// =============================================================================

console.log(`\n=== Results: ${testsPassed}/${testsRun} tests passed ===\n`);
process.exit(testsPassed === testsRun ? 0 : 1);
