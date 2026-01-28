# Agents Guide for FuelWise WASM

## Overview

FuelWise WASM is the WebAssembly build of the FuelWise optimization library, enabling browser-based refueling optimization without server dependencies.

## Directory Structure

```
wasm/
├── src/
│   ├── fuelwise_wasm.c     # WASM entry points
│   └── fuelwise-wrapper.js # JavaScript API wrapper
├── build/                  # Generated WASM files
│   ├── fuelwise.wasm       # WebAssembly binary
│   ├── fuelwise.js         # Emscripten glue code
│   └── fuelwise.d.ts       # TypeScript declarations
├── test.html               # Browser test page
├── Makefile
├── AGENTS.md               # This file
└── CLAUDE.md
```

## Build Requirements

- Emscripten SDK (emsdk)
- Make

```bash
# Install Emscripten (if not installed)
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh

# Build WASM
make          # Builds fuelwise.wasm + fuelwise.js
make clean    # Clean build artifacts
```

## WASM Entry Points

All exported functions in `fuelwise_wasm.c`:

| Function | Description |
|----------|-------------|
| `wasm_malloc(size)` | Allocate memory |
| `wasm_free(ptr)` | Free memory |
| `wasm_haversine(lat1,lon1,lat2,lon2)` | Distance between coordinates |
| `wasm_filter_stations(...)` | Filter stations to route |
| `wasm_solve_simple(...)` | Solve with constant consumption |
| `wasm_solve_segments(...)` | Solve with variable consumption |
| `wasm_optimize_route(...)` | Full optimization pipeline |

## JavaScript API

The `fuelwise-wrapper.js` provides a high-level API:

```javascript
import { FuelWise } from './fuelwise-wrapper.js';

const fw = await FuelWise.load();

// Calculate distance
const miles = fw.distance(34.05, -118.24, 33.45, -112.07);

// Filter stations to route
const snapped = fw.filterStations(stations, polyline, 5.0);

// Solve optimization
const solution = fw.solve({
  totalDistance: 500,
  tankCapacity: 100,
  currentFuel: 30,
  consumptionMpg: 8,
  minimumFuel: 10,
  stations: snappedStations
});

// Full optimization
const result = fw.optimize({
  stations: rawStations,
  route: polyline,
  tankCapacity: 100,
  currentFuel: 30,
  consumptionMpg: 8,
  minimumFuel: 10
});
```

## Memory Model

WASM uses linear memory. The wrapper handles:
1. Allocating memory for input arrays
2. Copying JavaScript data to WASM heap
3. Calling WASM functions
4. Reading results from WASM heap
5. Freeing allocated memory

```javascript
// Internal memory management (handled by wrapper)
const ptr = Module._wasm_malloc(size);
// ... copy data, call function ...
Module._wasm_free(ptr);
```

## Data Marshalling

### Stations Array
```c
// C struct layout (packed)
typedef struct {
    int32_t id;              // 4 bytes
    double distance;         // 8 bytes
    double price;            // 8 bytes
} WasmStation;               // 20 bytes total
```

### Polyline Array
```c
// Array of coordinate pairs
double polyline[] = {
    lat1, lon1,
    lat2, lon2,
    ...
};
```

### Solution Output
```c
typedef struct {
    int32_t status;
    int32_t num_stops;
    double total_cost;
    double remaining_fuel;
    // purchases array follows
} WasmSolution;
```

## Browser Usage

```html
<script type="module">
import { FuelWise } from './src/fuelwise-wrapper.js';

async function runOptimization() {
    const fw = await FuelWise.load();

    const result = fw.optimize({
        stations: [...],
        route: [...],
        tankCapacity: 300,
        currentFuel: 50,
        consumptionMpg: 6.5,
        minimumFuel: 25
    });

    console.log('Total cost:', result.totalCost);
    console.log('Stops:', result.stops);
}

runOptimization();
</script>
```

## Testing

1. Build the WASM module: `make`
2. Start a local server: `python -m http.server 8000`
3. Open `http://localhost:8000/test.html`
4. Check browser console for test results

## Common Tasks

### Adding a new WASM function

1. Add C function in `fuelwise_wasm.c`:
```c
EMSCRIPTEN_KEEPALIVE
double wasm_new_function(double arg1, int arg2) {
    return fw_some_function(arg1, arg2);
}
```

2. Add to Makefile exports:
```makefile
EXPORTED_FUNCTIONS += '_wasm_new_function'
```

3. Add JavaScript wrapper method:
```javascript
newFunction(arg1, arg2) {
    return this.module._wasm_new_function(arg1, arg2);
}
```

### Debugging WASM

1. Build with debug info: `make DEBUG=1`
2. Use browser DevTools Sources panel
3. Check for memory leaks with `console.log(Module.HEAP8.length)`

## Performance Notes

- WASM is ~2x slower than native C for this workload
- Initial load ~100ms for module instantiation
- Memory allocation is the main overhead
- Consider batching operations for many small calls

## Limitations

1. **No threading**: Single-threaded execution
2. **Memory limit**: Default 256MB heap
3. **No file I/O**: All data passed via API
4. **Browser-only**: Node.js support possible but not tested
