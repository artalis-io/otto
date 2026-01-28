# Velo WASM - Claude Instructions

## Overview

Velo WASM compiles the C routing library to WebAssembly for browser use. It wraps the Velo library with browser-friendly entry points.

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
| `src/velo_wasm.c` | WASM entry points |
| `Makefile` | Emscripten build |
| `build/velo.js` | Generated JS loader |
| `build/velo.wasm` | Generated WASM binary |

## Build Targets

```bash
make          # Build WASM module
make debug    # Build with debug info
make release  # Build optimized (smaller)
make types    # Generate TypeScript declarations
make test     # Test WASM build with Node.js
make clean    # Remove build artifacts
```

## WASM API

### Memory Management
```javascript
const ptr = Module._wasm_malloc(size);
Module._wasm_free(ptr);
```

### Graph Loading
```javascript
// Load binary graph from ArrayBuffer
const graphPtr = Module._wasm_load_graph_memory(dataPtr, size);
const nodeCount = Module._wasm_graph_node_count(graphPtr);
const edgeCount = Module._wasm_graph_edge_count(graphPtr);
Module._wasm_graph_free(graphPtr);
```

### Routing
```javascript
// Calculate route
// profile: 0=car, 1=truck, 2=bike, 3=foot
// mode: 0=fastest, 1=shortest
const routePtr = Module._wasm_route(graphPtr, fromLat, fromLon, toLat, toLon, profile, mode);
const distance = Module._wasm_route_distance(routePtr);  // meters
const duration = Module._wasm_route_duration(routePtr);  // seconds
const nodeCount = Module._wasm_route_node_count(routePtr);

// Get coordinates
const coordsPtr = Module._wasm_malloc(nodeCount * 2 * 8);
Module._wasm_route_get_coords(routePtr, coordsPtr);
// Read from HEAPF64...

Module._wasm_route_free(routePtr);
```

### Utilities
```javascript
// Find nearest node
const nodeIdx = Module._wasm_nearest_node(graphPtr, lat, lon);
```

## JavaScript Wrapper Example

```javascript
import Velo from './build/velo.js';

class VeloRouter {
  constructor() {
    this.module = null;
    this.graph = null;
  }

  async init() {
    this.module = await Velo();
  }

  loadGraph(arrayBuffer) {
    const data = new Uint8Array(arrayBuffer);
    const ptr = this.module._wasm_malloc(data.length);
    this.module.HEAPU8.set(data, ptr);
    this.graph = this.module._wasm_load_graph_memory(ptr, data.length);
    this.module._wasm_free(ptr);
    return this.graph !== 0;
  }

  route(from, to, profile = 0, mode = 0) {
    const routePtr = this.module._wasm_route(
      this.graph,
      from.lat, from.lon,
      to.lat, to.lon,
      profile, mode
    );

    if (!routePtr) return null;

    const result = {
      distance: this.module._wasm_route_distance(routePtr),
      duration: this.module._wasm_route_duration(routePtr),
      coordinates: this._getCoordinates(routePtr)
    };

    this.module._wasm_route_free(routePtr);
    return result;
  }

  _getCoordinates(routePtr) {
    const count = this.module._wasm_route_node_count(routePtr);
    const ptr = this.module._wasm_malloc(count * 2 * 8);
    this.module._wasm_route_get_coords(routePtr, ptr);

    const coords = [];
    for (let i = 0; i < count; i++) {
      coords.push({
        lat: this.module.HEAPF64[(ptr >> 3) + i * 2],
        lon: this.module.HEAPF64[(ptr >> 3) + i * 2 + 1]
      });
    }

    this.module._wasm_free(ptr);
    return coords;
  }

  destroy() {
    if (this.graph) {
      this.module._wasm_graph_free(this.graph);
      this.graph = null;
    }
  }
}
```

## Notes

- Graph must be loaded from binary `.vlg` format (not PBF)
- Use velo CLI to convert PBF to binary: `./velo map.osm.pbf -o map.vlg`
- Memory is managed manually - always free allocated pointers
- Coordinates are in lat/lon order
