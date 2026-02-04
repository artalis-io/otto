---
name: js-audit
description: Audit JavaScript code for security, WebGL resource management, WASM interop safety, and OTTO best practices. Use when reviewing ClayShards, WebGL renderers, or browser-side code.
user-invocable: true
---

# JavaScript Code Audit Skill

Perform comprehensive security, safety, and quality audits on OTTO JavaScript modules, focusing on WebGL rendering, WASM interop, and browser-side code.

**Target module:** $ARGUMENTS

## Usage

```
/js-audit <module>           # Audit a module (e.g., /js-audit clayshards)
/js-audit <module> --fix     # Audit and apply fixes
/js-audit <path/to/file.js>  # Audit a specific file
```

## Audit Categories

### 1. WASM-JavaScript Boundary Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unbounded memory access | `new Uint8Array(memory.buffer, ptr, len)` without bounds check | Critical |
| Missing null terminator | String passed to WASM without `\0` | High |
| Memory view invalidation | Holding `TypedArray` view across WASM calls that may grow memory | Critical |
| Type coercion errors | Passing JS number to WASM expecting i32/f64 without validation | High |
| Missing export validation | Calling WASM exports without checking they exist | Medium |
| Memory leak | `malloc()` without corresponding `free()` | High |

**Safe WASM String Patterns:**

```javascript
// BAD: Memory view can become invalid if WASM grows memory
const memory = new Uint8Array(wasm.memory.buffer);
wasm.someFunction();  // May grow memory!
memory.set(data, ptr);  // INVALID - view is detached!

// GOOD: Re-fetch memory view after any WASM call
const ptr = wasm.malloc(bytes.length);
const memory = new Uint8Array(wasm.memory.buffer);  // Fresh view
memory.set(bytes, ptr);

// BAD: No bounds check on memory read
function readString(ptr, len) {
    const bytes = new Uint8Array(wasm.memory.buffer, ptr, len);
    return new TextDecoder().decode(bytes);
}

// GOOD: Validate bounds before read
function readString(ptr, len) {
    const memory = wasm.memory.buffer;
    if (ptr < 0 || len < 0 || ptr + len > memory.byteLength) {
        throw new Error(`Invalid memory access: ptr=${ptr}, len=${len}, max=${memory.byteLength}`);
    }
    const bytes = new Uint8Array(memory, ptr, len);
    return new TextDecoder().decode(bytes);
}

// BAD: String without null terminator
function allocString(wasm, str) {
    const bytes = new TextEncoder().encode(str);
    const ptr = wasm.malloc(bytes.length);
    // Missing null terminator!
}

// GOOD: Always null-terminate strings for C/WASM
function allocString(wasm, str) {
    const bytes = new TextEncoder().encode(str + '\0');
    const ptr = wasm.malloc(bytes.length);
    const memory = new Uint8Array(wasm.memory.buffer);
    memory.set(bytes, ptr);
    return ptr;
}
```

**Memory Lifecycle Pattern:**

```javascript
// GOOD: Explicit allocation and cleanup
function withWasmString(wasm, str, callback) {
    const ptr = allocString(wasm, str);
    try {
        return callback(ptr);
    } finally {
        wasm.free(ptr);
    }
}

// Usage
const result = withWasmString(wasm, query, (ptr) => {
    return wasm.search(ptr);
});
```

### 2. WebGL Resource Management (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Shader compilation unchecked | `gl.compileShader()` without `gl.getShaderParameter(COMPILE_STATUS)` | High |
| Program link unchecked | `gl.linkProgram()` without `gl.getProgramParameter(LINK_STATUS)` | High |
| Texture leak | `gl.createTexture()` without corresponding `gl.deleteTexture()` | Medium |
| Buffer leak | `gl.createBuffer()` without corresponding `gl.deleteBuffer()` | Medium |
| Context loss ignored | No `webglcontextlost` event handler | High |
| Unbounded texture cache | Textures cached without eviction policy | Medium |

**Safe Shader Compilation:**

```javascript
// BAD: No error checking
function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    return shader;
}

// GOOD: Full error checking with cleanup
function createShader(gl, type, source) {
    const shader = gl.createShader(type);
    if (!shader) {
        throw new Error('Failed to create shader object');
    }

    gl.shaderSource(shader, source);
    gl.compileShader(shader);

    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        const info = gl.getShaderInfoLog(shader);
        gl.deleteShader(shader);
        throw new Error(`Shader compilation failed: ${info}`);
    }

    return shader;
}

// GOOD: Program creation with cleanup on failure
function createProgram(gl, vertexShader, fragmentShader) {
    const program = gl.createProgram();
    gl.attachShader(program, vertexShader);
    gl.attachShader(program, fragmentShader);
    gl.linkProgram(program);

    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
        const info = gl.getProgramInfoLog(program);
        gl.deleteProgram(program);
        throw new Error(`Program linking failed: ${info}`);
    }

    return program;
}
```

**Context Loss Handling:**

```javascript
// GOOD: Handle context loss gracefully
canvas.addEventListener('webglcontextlost', (e) => {
    e.preventDefault();  // Allow context restoration
    this.contextLost = true;
    // Cancel animation frame, stop rendering
    if (this.animationFrameId) {
        cancelAnimationFrame(this.animationFrameId);
    }
});

canvas.addEventListener('webglcontextrestored', () => {
    this.contextLost = false;
    this._initWebGL();  // Recreate all resources
    this._startRenderLoop();
});

// In render loop
render() {
    if (this.contextLost) return;  // Early exit if context lost
    // ... rendering code
}
```

**Texture Cache with Eviction:**

```javascript
// BAD: Unbounded cache
const textureCache = new Map();  // Grows forever

// GOOD: LRU cache with size limit
class TextureCache {
    constructor(gl, maxSize = 256) {
        this.gl = gl;
        this.maxSize = maxSize;
        this.cache = new Map();
    }

    get(key) {
        const entry = this.cache.get(key);
        if (entry) {
            // Move to end (most recently used)
            this.cache.delete(key);
            this.cache.set(key, entry);
        }
        return entry?.texture;
    }

    set(key, texture) {
        // Evict oldest if at capacity
        while (this.cache.size >= this.maxSize) {
            const [oldestKey, oldest] = this.cache.entries().next().value;
            this.gl.deleteTexture(oldest.texture);
            this.cache.delete(oldestKey);
        }
        this.cache.set(key, { texture, timestamp: Date.now() });
    }

    clear() {
        for (const entry of this.cache.values()) {
            this.gl.deleteTexture(entry.texture);
        }
        this.cache.clear();
    }
}
```

### 3. URL and API Security (High)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| URL injection | Template literal with user input in URL path | High |
| Missing encodeURIComponent | Query parameter from user without encoding | High |
| HTTP in production | `http://` URLs for external resources | Medium |
| Hardcoded credentials | API keys in client-side code | Critical |
| Open redirect | User-controlled redirect URL | High |
| CORS bypass attempt | `mode: 'no-cors'` with sensitive data | Medium |

**Safe URL Construction:**

```javascript
// BAD: Direct string interpolation
const url = `${server}/api/v1/search?q=${query}`;  // XSS if query contains & or =

// GOOD: Always encode user input
const url = `${server}/api/v1/search?q=${encodeURIComponent(query)}`;

// BAD: User-controlled path segment
const url = `${server}/tiles/${userZoom}/${userX}/${userY}.png`;

// GOOD: Validate and constrain values
function getTileUrl(server, z, x, y) {
    // Validate zoom level
    z = Math.floor(z);
    if (z < 0 || z > 22) {
        throw new Error(`Invalid zoom: ${z}`);
    }

    // Constrain tile coordinates
    const maxTile = Math.pow(2, z);
    x = Math.floor(x);
    y = Math.floor(y);
    if (x < 0 || x >= maxTile || y < 0 || y >= maxTile) {
        throw new Error(`Invalid tile coordinates: ${x},${y} at zoom ${z}`);
    }

    return `${server}/tiles/${z}/${x}/${y}.png`;
}

// BAD: API key in client code
const API_KEY = 'sk-live-abc123';  // Exposed in browser!

// GOOD: Use environment-injected or server-proxied keys
// Or use public keys only (rate-limited, domain-restricted)
```

### 4. API Client Resilience (High)

When making HTTP requests to APIs (routing, geocoding, tile servers), handle backpressure and failures gracefully.

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| No retry logic | Single `fetch()` without retry | High |
| No backoff | Immediate retry on failure | High |
| Missing status checks | Not handling 429/503/504 | High |
| No circuit breaker | Hammering failing service | Medium |
| Infinite retries | Retry loop without max attempts | High |
| No timeout | `fetch()` without AbortController | Medium |
| Thundering herd | All clients retry at same time | Medium |

**Shared Library APIs (Preferred):**

OTTO provides production-ready implementations in `shared/js/`:
- `CircuitBreaker` - Circuit breaker class with CLOSED/OPEN/HALF_OPEN states
- `createBackoff()` - Stateful exponential backoff iterator
- `calculateBackoff()` - Stateless backoff calculation
- `createResilientFetch()` - Fetch wrapper with retry + circuit breaker
- `isRetryableStatus()` - Check if HTTP status code is retryable

```javascript
import {
    createResilientFetch,
    CircuitBreaker,
    createBackoff,
    isRetryableStatus
} from '../../shared/js/index.js';

// Create resilient fetch client with circuit breaker
const client = createResilientFetch({
    circuit: new CircuitBreaker({ failureThreshold: 5 }),
    timeoutMs: 30000,
    maxRetries: 3,
    baseDelayMs: 100,
    maxDelayMs: 10000
});

// Make requests with automatic retry and circuit breaker
try {
    const response = await client.fetch('/api/v1/route?from=...');
    const data = await response.json();
} catch (err) {
    if (err.isCircuitOpen) {
        // Circuit is open - service is unhealthy
        console.error('Service unavailable');
    } else if (err.isTimeout) {
        console.error('Request timed out');
    } else {
        console.error('Request failed:', err.message);
    }
}

// Low-level usage for custom control
const backoff = createBackoff({ maxRetries: 5 });
while (backoff.hasRetries()) {
    try {
        const response = await fetch(url);
        if (response.ok) break;
        if (!isRetryableStatus(response.status)) throw new Error(`HTTP ${response.status}`);
        await backoff.wait();  // Wait and advance attempt counter
    } catch (err) {
        await backoff.wait();
    }
}
```

**MapProvider Integration (ClayShards):**

```javascript
import { MapProvider } from './clay-shards-webgl/index.js';

// MapProvider now uses resilient-fetch internally
const provider = new MapProvider(wasm, {
    timeoutMs: 30000,
    maxRetries: 3,
    circuitFailureThreshold: 5
});

// Check circuit states
console.log('Route circuit:', provider.getRouteCircuitState());     // 'closed'|'open'|'half_open'
console.log('Geocode circuit:', provider.getGeocodeCircuitState());
```

**Manual Implementation (for reference):**

**HTTP Status Code Handling:**

```javascript
// Status codes that indicate backpressure
const BACKPRESSURE_CODES = new Set([429, 503, 504, 502]);
const RETRY_CODES = new Set([429, 500, 502, 503, 504]);

function classifyResponse(response) {
    if (response.ok) return 'success';
    if (response.status === 429) return 'rate_limited';
    if (response.status === 503) return 'overloaded';
    if (response.status === 504) return 'timeout';
    if (response.status >= 500) return 'server_error';
    return 'client_error';  // 4xx - don't retry
}
```

**Exponential Backoff with Jitter:**

```javascript
// Configurable retry settings
const DEFAULT_RETRY_CONFIG = {
    baseDelayMs: 100,       // Initial delay
    maxDelayMs: 30000,      // Maximum delay cap
    maxRetries: 5,          // Maximum attempts
    jitterFactor: 0.2,      // Randomization (0.0-1.0)
};

function calculateBackoff(attempt, config = DEFAULT_RETRY_CONFIG) {
    // Exponential: base * 2^attempt
    let delay = config.baseDelayMs * Math.pow(2, attempt);
    delay = Math.min(delay, config.maxDelayMs);

    // Add jitter to prevent thundering herd
    const jitter = config.jitterFactor;
    const factor = 1 - jitter + Math.random() * 2 * jitter;

    return Math.floor(delay * factor);
}

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}
```

**Fetch with Retry and Backoff:**

```javascript
async function fetchWithRetry(url, options = {}, retryConfig = DEFAULT_RETRY_CONFIG) {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), options.timeout || 30000);

    let lastError;

    for (let attempt = 0; attempt <= retryConfig.maxRetries; attempt++) {
        try {
            const response = await fetch(url, {
                ...options,
                signal: controller.signal,
            });

            clearTimeout(timeoutId);

            // Check for backpressure
            if (response.status === 429) {
                const retryAfter = response.headers.get('Retry-After');
                const delay = retryAfter
                    ? parseInt(retryAfter, 10) * 1000
                    : calculateBackoff(attempt, retryConfig);

                if (attempt < retryConfig.maxRetries) {
                    await sleep(delay);
                    continue;
                }
            }

            // Retry on server errors
            if (RETRY_CODES.has(response.status) && attempt < retryConfig.maxRetries) {
                await sleep(calculateBackoff(attempt, retryConfig));
                continue;
            }

            return response;

        } catch (err) {
            clearTimeout(timeoutId);
            lastError = err;

            if (err.name === 'AbortError') {
                throw new Error(`Request timeout: ${url}`);
            }

            // Network error - retry with backoff
            if (attempt < retryConfig.maxRetries) {
                await sleep(calculateBackoff(attempt, retryConfig));
                continue;
            }
        }
    }

    throw lastError || new Error(`Max retries exceeded: ${url}`);
}
```

**Circuit Breaker Pattern (for reference):**

```javascript
class CircuitBreaker {
    constructor(options = {}) {
        this.failureThreshold = options.failureThreshold || 5;
        this.successThreshold = options.successThreshold || 3;
        this.openDurationMs = options.openDurationMs || 30000;

        this.state = 'closed';  // 'closed' | 'open' | 'half-open'
        this.failureCount = 0;
        this.successCount = 0;
        this.lastFailureTime = 0;
    }

    async execute(fn) {
        if (!this.allowRequest()) {
            throw new Error('Circuit breaker is open');
        }

        try {
            const result = await fn();
            this.recordSuccess();
            return result;
        } catch (err) {
            this.recordFailure();
            throw err;
        }
    }

    allowRequest() {
        if (this.state === 'closed') return true;

        if (this.state === 'open') {
            const now = Date.now();
            if (now - this.lastFailureTime > this.openDurationMs) {
                this.state = 'half-open';
                this.successCount = 0;
                return true;
            }
            return false;  // Fail fast
        }

        return true;  // half-open: allow test request
    }

    recordSuccess() {
        this.failureCount = 0;

        if (this.state === 'half-open') {
            this.successCount++;
            if (this.successCount >= this.successThreshold) {
                this.state = 'closed';
            }
        }
    }

    recordFailure() {
        this.failureCount++;
        this.lastFailureTime = Date.now();

        if (this.state === 'half-open') {
            this.state = 'open';
        } else if (this.failureCount >= this.failureThreshold) {
            this.state = 'open';
        }
    }

    getState() {
        return {
            state: this.state,
            failureCount: this.failureCount,
            successCount: this.successCount,
        };
    }
}

// Usage
const routingCircuit = new CircuitBreaker({
    failureThreshold: 5,
    openDurationMs: 30000,
});

async function getRoute(from, to) {
    return routingCircuit.execute(async () => {
        const response = await fetchWithRetry(`/api/v1/route?from=${from}&to=${to}`);
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.json();
    });
}
```

**CORS Handling (Client-Side):**

```javascript
// GOOD: Handle CORS errors gracefully
async function fetchCrossOrigin(url, options = {}) {
    try {
        const response = await fetch(url, {
            ...options,
            mode: 'cors',  // Explicit CORS mode
            credentials: options.credentials || 'omit',
        });
        return response;
    } catch (err) {
        // CORS errors throw TypeError with no useful message
        if (err instanceof TypeError && err.message === 'Failed to fetch') {
            throw new Error(`CORS error or network failure for: ${url}`);
        }
        throw err;
    }
}

// For tile servers that may not support CORS
function loadTileImage(url) {
    return new Promise((resolve, reject) => {
        const img = new Image();
        img.crossOrigin = 'anonymous';  // Request CORS

        img.onload = () => resolve(img);
        img.onerror = () => {
            // Fallback: try without CORS (won't work for WebGL textures)
            console.warn(`CORS failed for tile: ${url}`);
            reject(new Error(`Failed to load tile: ${url}`));
        };

        img.src = url;
    });
}
```

**Configuration via Options:**

```javascript
// Allow configuration of resilience settings
const API_CONFIG = {
    routing: {
        baseUrl: '/api/v1',
        retry: { maxRetries: 3, baseDelayMs: 200 },
        circuit: { failureThreshold: 5, openDurationMs: 60000 },
        timeout: 10000,
    },
    tiles: {
        baseUrl: 'https://tiles.example.com',
        retry: { maxRetries: 2, baseDelayMs: 100 },
        circuit: { failureThreshold: 10, openDurationMs: 30000 },
        timeout: 5000,
    },
    geocoding: {
        baseUrl: '/api/v1',
        retry: { maxRetries: 2, baseDelayMs: 100 },
        circuit: { failureThreshold: 5, openDurationMs: 30000 },
        timeout: 5000,
    },
};
```

**API Client Audit Checklist:**

| Check | Severity | Description |
|-------|----------|-------------|
| 429 handling | High | Backs off on rate limit, respects Retry-After |
| 503/504 handling | High | Retries with exponential backoff |
| Circuit breaker | High | Fails fast when API is unhealthy |
| Max retries | High | Bounded retry count, not infinite |
| Backoff jitter | Medium | Randomized delays prevent thundering herd |
| Request timeout | Medium | AbortController with reasonable timeout |
| CORS handling | Medium | Graceful error for CORS failures |
| Configurable | Low | Retry/circuit/timeout settings exposed |

### 5. Input Handling Security (Medium)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Clipboard without permission | `navigator.clipboard.readText()` without try/catch | Medium |
| Unthrottled event handlers | Mouse/touch handlers without debounce | Low |
| Event listener leak | `addEventListener` without corresponding `removeEventListener` | Medium |
| Keyboard event conflicts | Capturing browser shortcuts (Ctrl+S, etc.) | Low |

**Safe Clipboard Handling:**

```javascript
// BAD: No error handling
async function paste() {
    const text = await navigator.clipboard.readText();
    process(text);
}

// GOOD: Handle permission denial and errors
async function paste() {
    try {
        const text = await navigator.clipboard.readText();
        // Sanitize clipboard content before use
        const sanitized = text.slice(0, MAX_PASTE_LENGTH);
        process(sanitized);
    } catch (err) {
        if (err.name === 'NotAllowedError') {
            console.warn('Clipboard access denied by user');
            return;
        }
        console.error('Clipboard read failed:', err);
    }
}
```

**Event Listener Lifecycle:**

```javascript
// BAD: Listener never removed
window.addEventListener('resize', this.handleResize);

// GOOD: Track and cleanup listeners
class Renderer {
    constructor() {
        this._boundResize = this.handleResize.bind(this);
        window.addEventListener('resize', this._boundResize);
    }

    destroy() {
        window.removeEventListener('resize', this._boundResize);
    }
}
```

### 6. Async/Promise Error Handling (Medium)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Unhandled rejection | `async` function without try/catch or .catch() | Medium |
| Silent failure | catch block with only console.log | Medium |
| Missing finally | Resource cleanup not in finally block | Medium |
| Race condition | Multiple async operations without proper sequencing | Medium |

**Safe Async Patterns:**

```javascript
// BAD: Unhandled rejection
async function loadData() {
    const response = await fetch(url);  // Can throw!
    const data = await response.json();
    return data;
}

// GOOD: Explicit error handling
async function loadData() {
    try {
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`HTTP ${response.status}: ${response.statusText}`);
        }
        return await response.json();
    } catch (err) {
        if (err.name === 'AbortError') {
            return null;  // Request was cancelled, not an error
        }
        throw new Error(`Failed to load data: ${err.message}`);
    }
}

// BAD: Silent failure
fetch(url).catch(err => console.log(err));

// GOOD: Propagate or handle meaningfully
fetch(url)
    .then(handleResponse)
    .catch(err => {
        showUserError('Failed to load. Please try again.');
        reportError(err);  // Send to error tracking
    });
```

### 7. Data Validation (Medium)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Missing JSON schema validation | `JSON.parse()` without structure validation | Medium |
| Unbounded loop | `while` loop over external data without limit | High |
| NaN/Infinity propagation | Math operations without isFinite check | Medium |
| Type coercion bugs | `==` instead of `===` | Low |

**Safe Data Parsing:**

```javascript
// BAD: No validation
function loadFontData(json) {
    return JSON.parse(json);
}

// GOOD: Validate expected structure
function loadFontData(json) {
    const data = JSON.parse(json);

    if (!data || typeof data !== 'object') {
        throw new Error('Font data must be an object');
    }
    if (!Array.isArray(data.glyphs)) {
        throw new Error('Font data missing glyphs array');
    }
    if (!data.atlas || typeof data.atlas.width !== 'number') {
        throw new Error('Font data missing atlas dimensions');
    }

    return data;
}

// BAD: Unbounded loop
function decodePolyline(encoded) {
    let index = 0;
    const points = [];
    while (index < encoded.length) {
        // ... decode point
        points.push(point);
    }
    return points;
}

// GOOD: Iteration limit
function decodePolyline(encoded, maxPoints = 100000) {
    let index = 0;
    const points = [];
    while (index < encoded.length && points.length < maxPoints) {
        // ... decode point
        points.push(point);
    }
    if (points.length >= maxPoints) {
        console.warn(`Polyline truncated at ${maxPoints} points`);
    }
    return points;
}

// BAD: NaN can propagate
function calculateZoom(width, bounds) {
    return Math.log2(width / (bounds.east - bounds.west));  // NaN if bounds invalid
}

// GOOD: Validate inputs
function calculateZoom(width, bounds) {
    const span = bounds.east - bounds.west;
    if (!isFinite(span) || span <= 0 || !isFinite(width) || width <= 0) {
        throw new Error('Invalid bounds or width for zoom calculation');
    }
    return Math.log2(width / span);
}
```

### 8. Performance Patterns (Low)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Forced layout thrashing | Read then write DOM in loop | Medium |
| Missing requestAnimationFrame | Direct DOM updates on scroll/resize | Low |
| Unbounded polling | setInterval without clear condition | Low |
| Large closure capture | Anonymous functions capturing entire scope | Low |

**Safe Animation Patterns:**

```javascript
// BAD: Polling without cleanup
const interval = setInterval(() => checkForUpdates(), 50);

// GOOD: Conditional polling with cleanup
class Poller {
    constructor(callback, interval = 50) {
        this.callback = callback;
        this.interval = interval;
        this.timerId = null;
    }

    start() {
        if (this.timerId) return;
        this.timerId = setInterval(() => this.callback(), this.interval);
    }

    stop() {
        if (this.timerId) {
            clearInterval(this.timerId);
            this.timerId = null;
        }
    }
}

// BAD: Render without frame budget
function render() {
    // Heavy work without checking time budget
    for (const item of items) {
        renderItem(item);
    }
    requestAnimationFrame(render);
}

// GOOD: Frame budget aware rendering
function render(timestamp) {
    const budget = 16;  // ~60fps
    const start = performance.now();

    while (pendingItems.length > 0) {
        if (performance.now() - start > budget) {
            break;  // Yield to next frame
        }
        renderItem(pendingItems.shift());
    }

    requestAnimationFrame(render);
}
```

### 9. OTTO-Specific Patterns

#### Naming Conventions

| Component | Prefix | Example |
|-----------|--------|---------|
| ClayShards | `cs_`, `CS_` | `cs_button()`, `CS_FOCUS_NONE` |
| Renderer | `Clay` | `ClayRenderer`, `ClayFont` |
| Map | `map_`, `Map` | `MapProvider`, `map_overlays` |

#### Module Structure

```javascript
// GOOD: Clean module exports
export { ClayRenderer } from './renderer.js';
export { MSDFFont } from './font.js';
export { loadWasm } from './wasm-loader.js';

// GOOD: Named exports for tree-shaking
export function createRenderer(canvas, wasm) { ... }
export function loadFont(url) { ... }
```

#### WASM Export Validation

```javascript
// GOOD: Validate all required exports at load time
const REQUIRED_EXPORTS = [
    'malloc', 'free', 'memory',
    'cs_init', 'cs_frame_begin', 'cs_frame_end',
    'Clay_GetRenderCommands', 'Clay_RenderCommandArray_length',
];

function validateWasmExports(wasm) {
    for (const name of REQUIRED_EXPORTS) {
        if (!(name in wasm)) {
            throw new Error(`Missing required WASM export: ${name}`);
        }
    }
}
```

### 10. Test Coverage Checklist

Check test file for:
- [ ] WASM memory boundary edge cases
- [ ] WebGL context loss recovery
- [ ] Network failure handling
- [ ] Invalid input handling (NaN, null, undefined)
- [ ] Event listener cleanup verification
- [ ] Cache eviction behavior
- [ ] Large data handling (100K+ points)

## Audit Procedure

When `/js-audit <module>` is invoked:

1. **Locate Files**
   ```
   <module>/
   ├── *.js           # JavaScript modules
   ├── index.js       # Entry point
   └── tests/         # Test files (if any)
   ```

2. **Scan for Critical Issues**
   - Search for WASM memory access patterns
   - Search for unchecked WebGL operations
   - Search for URL construction with user input

3. **Review Module Exports**
   - Check all exported functions
   - Verify WASM export validation
   - Check for proper cleanup methods

4. **Check Resource Management**
   - WebGL resources created vs deleted
   - Event listeners added vs removed
   - Timers/intervals started vs stopped

5. **Verify Error Handling**
   - All async functions have try/catch or .catch()
   - User-facing errors are meaningful
   - Network failures handled gracefully

6. **Generate Report**
   Format: Markdown table with findings, severity, file:line, and suggested fix

## Report Format

```markdown
## JS Audit Report: <module>

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| File:Line | Issue | Current Code | Suggested Fix |
|-----------|-------|--------------|---------------|
| renderer.js:42 | Memory view invalidation | `memory.set(data, ptr)` after WASM call | Re-fetch memory view |

### High Issues
...

### Recommendations

1. **WASM Safety**: ...
2. **WebGL Resources**: ...
3. **Error Handling**: ...
```

## Fix Mode (--fix)

When `--fix` is specified:

1. Generate the audit report first
2. For each fixable issue, apply the transformation
3. Show diff of changes
4. Re-run tests if available
5. Report any test failures

**Auto-fixable Issues:**
- Missing `encodeURIComponent` on URL parameters
- Missing `try/catch` around async clipboard operations
- Missing WebGL compile/link status checks
- `==` to `===` conversions
- Missing `isFinite()` checks on calculations

**NOT Auto-fixable (require manual review):**
- WASM memory boundary redesign
- WebGL resource lifecycle changes
- Error handling strategy decisions
- Cache eviction policy design

## Key Files by Audit Category

| Audit Area | Primary Files |
|------------|---------------|
| WASM Interop | `renderer.js`, `keyboard.js`, `map-provider.js`, `demo.js` |
| WebGL | `renderer.js`, `map-tiles.js`, `map-overlays.js`, `shaders.js` |
| External APIs | `map-provider.js`, `map-tiles.js` |
| Input Handling | `keyboard.js`, `demo.js`, `render-loop.js` |
| Font Loading | `font.js`, `utils.js` |
| State Management | `demo.js`, `map-provider.js` |

## Checklist Summary

Before marking a module as "audited":

**WASM Safety:**
- [ ] All memory views re-fetched after WASM calls that may grow memory
- [ ] All string allocations include null terminator
- [ ] All memory reads bounds-checked
- [ ] All malloc calls have corresponding free

**WebGL Resources:**
- [ ] Shader compile status checked
- [ ] Program link status checked
- [ ] Context loss handled
- [ ] Texture cache has eviction policy
- [ ] All resources cleaned up on destroy

**URL/API Safety:**
- [ ] All user input in URLs encoded with `encodeURIComponent`
- [ ] No hardcoded API keys
- [ ] HTTPS used for external resources
- [ ] CORS errors handled gracefully

**API Client Resilience:**
- [ ] Uses `createResilientFetch()` from `shared/js/` or equivalent
- [ ] Uses `CircuitBreaker` from `shared/js/` for circuit breaker protection
- [ ] Uses `createBackoff()` or `calculateBackoff()` for exponential backoff
- [ ] HTTP 429/503/504 handled with backoff (via `isRetryableStatus()`)
- [ ] Maximum retry count bounded
- [ ] Request timeouts via AbortController
- [ ] Retry/circuit settings configurable
- [ ] MapProvider uses resilient-fetch internally

**Error Handling:**
- [ ] All async functions have error handling
- [ ] Network failures show user-friendly messages
- [ ] No silent catch blocks

**Input Handling:**
- [ ] Clipboard operations have permission handling
- [ ] Event listeners removed on cleanup
- [ ] Input data validated before use

**Performance:**
- [ ] Render loop uses requestAnimationFrame
- [ ] Polling has stop condition
- [ ] Caches have size limits
