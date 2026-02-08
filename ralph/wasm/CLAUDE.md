# Ralph WASM - Claude Instructions

## Overview

Ralph WASM compiles the LP/MIP solver to WebAssembly for browser use. Unlike Carta/Velo/Locus, Ralph is stateless - no map data to embed. The WASM module accepts LP/MPS problems and returns solutions.

## Quick Start

```bash
# Requires Emscripten installed
source /path/to/emsdk/emsdk_env.sh
make          # Build WASM
make test     # Test WASM build
```

## Key Files

| File | Purpose |
|------|---------|
| `src/ralph_wasm_api.c` | WASM API wrapper |
| `Makefile` | Emscripten build |
| `build/ralph-api-demo.js` | Generated JS (single-file, ~370KB) |

## Build Targets

```bash
make          # Build API demo (single-file JS)
make debug    # Build with debug info
make release  # Build optimized (smaller)
make test     # Test WASM build with Node.js
make clean    # Remove build artifacts
```

## WASM API

### Initialization

```javascript
const RalphAPIDemo = require('./ralph-api-demo.js');
const m = await RalphAPIDemo();

// Initialize (call once)
m._ralph_api_init();

// Check if ready
if (m._ralph_wasm_api_ready()) {
    console.log('Ready!');
}

// Get version
const version = m.UTF8ToString(m._ralph_wasm_api_version());
```

### Solving Problems

```javascript
// Prepare request
const method = m.allocateUTF8('POST');
const path = m.allocateUTF8('/api/v1/solve');
const body = m.allocateUTF8(JSON.stringify({
    format: 'lp',
    problem: `
        max: 5 x + 3 y
        subject to
        wood: 2 x + 4 y <= 40
        labor: 3 x + 2 y <= 24
        bounds
        x >= 0
        y >= 0
        end
    `
}));
const bodyLen = m.lengthBytesUTF8(body);

// Call API
const respPtr = m._ralph_wasm_api_handle(method, path, 0, body, bodyLen);

// Read response
if (respPtr) {
    const status = m._ralph_wasm_response_status(respPtr);
    const bodyPtr = m._ralph_wasm_response_body(respPtr);
    const response = m.UTF8ToString(bodyPtr);
    console.log('Status:', status);
    console.log('Response:', JSON.parse(response));
}

// Cleanup
m._free(method);
m._free(path);
m._free(body);
```

### Response Format

```json
{
    "status": "optimal",
    "objective": 40,
    "variables": {"x": 8, "y": 0},
    "solve_time_ms": 0.1,
    "iterations": 1,
    "num_vars": 2,
    "num_cons": 2
}
```

### Shutdown

```javascript
m._ralph_wasm_api_free();
```

## Exported Functions

| Function | Description |
|----------|-------------|
| `_ralph_api_init()` | Initialize API (call once) |
| `_ralph_wasm_api_free()` | Free API context |
| `_ralph_wasm_api_ready()` | Check if ready (returns 1/0) |
| `_ralph_wasm_api_handle(method, path, query, body, len)` | Handle request |
| `_ralph_wasm_response_status(resp)` | Get HTTP status |
| `_ralph_wasm_response_content_type(resp)` | Get content type |
| `_ralph_wasm_response_body(resp)` | Get body pointer |
| `_ralph_wasm_response_body_len(resp)` | Get body length |
| `_ralph_wasm_api_version()` | Get version string |
| `_malloc(size)` | Allocate memory |
| `_free(ptr)` | Free memory |

## Limitations

The WASM build has intentional size limits for demo use:

### Size Limits
- LP: 100 variables, 100 constraints
- MIP: 50 variables, 50 constraints

### Timeout
- Default: 5 seconds
- Maximum: 30 seconds

### Features Included
All Ralph solvers are included:
- Primal/dual simplex for LP
- Branch and bound for MIP
- LAP solver (Jonker-Volgenant-Castanon)
- Network simplex for MCNF
- Problem structure detection (LAP, network, SCP)
- Lagrangian relaxation for SCP

## JavaScript Wrapper Example

```javascript
class RalphSolver {
    constructor() {
        this.module = null;
    }

    async init() {
        const RalphAPIDemo = await import('./ralph-api-demo.js');
        this.module = await RalphAPIDemo.default();
        this.module._ralph_api_init();
    }

    solve(lpProblem) {
        const m = this.module;

        const method = m.allocateUTF8('POST');
        const path = m.allocateUTF8('/api/v1/solve');
        const body = m.allocateUTF8(JSON.stringify({
            format: 'lp',
            problem: lpProblem
        }));
        const bodyLen = m.lengthBytesUTF8(body);

        try {
            const respPtr = m._ralph_wasm_api_handle(method, path, 0, body, bodyLen);
            if (!respPtr) return null;

            const status = m._ralph_wasm_response_status(respPtr);
            const bodyPtr = m._ralph_wasm_response_body(respPtr);
            const response = m.UTF8ToString(bodyPtr);

            return {
                httpStatus: status,
                ...JSON.parse(response)
            };
        } finally {
            m._free(method);
            m._free(path);
            m._free(body);
        }
    }

    destroy() {
        if (this.module) {
            this.module._ralph_wasm_api_free();
            this.module = null;
        }
    }
}

// Usage
const solver = new RalphSolver();
await solver.init();

const result = solver.solve(`
    max: 5 x + 3 y
    subject to
    c1: 2 x + 4 y <= 40
    c2: 3 x + 2 y <= 24
    end
`);

console.log(result);
// { httpStatus: 200, status: 'optimal', objective: 40, variables: { x: 8, y: 0 }, ... }

solver.destroy();
```

## Dependencies

- **Emscripten SDK** - WebAssembly compiler
- No runtime dependencies (single-file build)

## Notes

- Single-file build: WASM embedded in JS (no separate .wasm file)
- Works offline: No network requests needed
- Memory: Uses ALLOW_MEMORY_GROWTH for flexible sizing
- Initial memory: 16MB (grows as needed)
