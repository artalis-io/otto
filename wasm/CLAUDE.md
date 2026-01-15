# Claude Code Instructions for FuelWise WASM

## Overview

FuelWise WASM compiles the C optimization library to WebAssembly for browser use. It wraps the FuelWise library with browser-friendly entry points.

## Quick Start

```bash
# Requires Emscripten installed
source /path/to/emsdk/emsdk_env.sh
make          # Build WASM
```

## Key Files

| File | Purpose |
|------|---------|
| `src/fuelwise_wasm.c` | WASM entry points |
| `src/fuelwise-wrapper.js` | JavaScript API |
| `test.html` | Browser test page |
| `Makefile` | Emscripten build |

## Build System

The Makefile:
1. Compiles all C sources (ralph + fuelwise + wasm bindings)
2. Links with Emscripten to produce `.wasm` + `.js`
3. Generates TypeScript declarations

Key build flags:
```makefile
CFLAGS = -O3 -s WASM=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue"]'
CFLAGS += -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s MODULARIZE=1
```

## WASM Entry Point Pattern

Each WASM function follows this pattern:

```c
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE
double wasm_function_name(/* simple types only */) {
    // Convert to internal types
    // Call FuelWise functions
    // Return result
}
```

Rules:
- Use `EMSCRIPTEN_KEEPALIVE` to prevent dead code elimination
- Only primitive types in function signatures
- Arrays passed as pointers + length
- Structs must be manually marshalled

## Memory Management

WASM has linear memory. Pattern for arrays:

```c
// JavaScript side
const ptr = Module._wasm_malloc(count * sizeof);
// Copy data to ptr using setValue()
const result = Module._wasm_function(ptr, count);
Module._wasm_free(ptr);
```

```c
// C side
EMSCRIPTEN_KEEPALIVE
void* wasm_malloc(int size) {
    return malloc(size);
}

EMSCRIPTEN_KEEPALIVE
void wasm_free(void* ptr) {
    free(ptr);
}
```

## Common Tasks

### Adding variable consumption support

Already implemented in `wasm_solve_segments()`:
```c
EMSCRIPTEN_KEEPALIVE
int wasm_solve_segments(
    double total_distance,
    double tank_capacity,
    double current_fuel,
    double minimum_fuel,
    double* stations_data,    // [id, dist, price, ...]
    int num_stations,
    double* segments_data,    // [start, weight, mpg, ...]
    int num_segments,
    double* solution_out
);
```

### Modifying the JavaScript wrapper

The wrapper in `fuelwise-wrapper.js` provides:
1. Module loading with `FuelWise.load()`
2. Data marshalling (JS objects <-> WASM memory)
3. Error handling
4. Type conversion

Example wrapper method:
```typescript
solve(config) {
    // Allocate WASM memory
    const stationsPtr = this._allocateStations(config.stations);

    // Call WASM function
    const resultPtr = this.module._wasm_solve_simple(
        config.totalDistance,
        config.tankCapacity,
        // ...
        stationsPtr,
        config.stations.length
    );

    // Read result
    const solution = this._readSolution(resultPtr);

    // Free memory
    this.module._wasm_free(stationsPtr);

    return solution;
}
```

### Testing in browser

1. Build: `make`
2. Start server: `python -m http.server 8000`
3. Open `http://localhost:8000/test.html`
4. Check DevTools console

## Debugging

### Build with debug symbols
```bash
make DEBUG=1
```

### Check memory usage
```typescript
console.log('Heap size:', Module.HEAP8.length);
```

### Trace function calls
Add logging in C:
```c
#include <stdio.h>
printf("wasm_solve: distance=%.2f, capacity=%.2f\n",
       total_distance, tank_capacity);
```

## Code Style

- `wasm_` prefix for all exported functions
- Use `EMSCRIPTEN_KEEPALIVE` macro
- Document memory ownership in comments
- Match JavaScript API to REST API where possible

## Integration with UI

The React UI can use WASM directly:

```typescript
// ui/src/services/fuelwiseWasm.ts
import { FuelWise } from '../../wasm/src/fuelwise-wrapper.js';

let fuelwise: FuelWise | null = null;

export async function loadWasm() {
    if (!fuelwise) {
        fuelwise = await FuelWise.load();
    }
    return fuelwise;
}

export async function optimizeLocal(request: OptimizeRequest) {
    const fw = await loadWasm();
    return fw.optimize(request);
}
```
