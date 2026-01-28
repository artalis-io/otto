# Carta WASM - Claude Instructions

## Overview

Carta WASM compiles the C tile generator library to WebAssembly for browser use. It wraps the Carta library with browser-friendly entry points for generating map tiles.

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
| `src/carta_wasm.c` | WASM entry points |
| `Makefile` | Emscripten build |
| `build/carta.js` | Generated JS loader |
| `build/carta.wasm` | Generated WASM binary |

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

### PBF Loading
```javascript
// Load PBF from ArrayBuffer
const ctxPtr = Module._wasm_load_pbf_memory(dataPtr, size);
const nodeCount = Module._wasm_context_node_count(ctxPtr);
const wayCount = Module._wasm_context_way_count(ctxPtr);

// Get bounding box
const bboxPtr = Module._wasm_malloc(4 * 8);
Module._wasm_context_bbox(ctxPtr, bboxPtr);
// bbox: [min_lat, min_lon, max_lat, max_lon]

Module._wasm_context_free(ctxPtr);
```

### Tile Generation
```javascript
// Generate MVT vector tile
const bufferSize = 1024 * 1024;  // 1MB
const bufferPtr = Module._wasm_malloc(bufferSize);
const mvtSize = Module._wasm_generate_mvt(ctxPtr, z, x, y, bufferPtr, bufferSize);
// Copy from bufferPtr...

// Generate PNG raster tile
const tileSize = 512;  // pixels
const pngSize = Module._wasm_generate_png(ctxPtr, z, x, y, tileSize, bufferPtr, bufferSize);
// Copy from bufferPtr...
```

### Utilities
```javascript
// Get tile bounds
const boundsPtr = Module._wasm_malloc(4 * 8);
Module._wasm_tile_bounds(z, x, y, boundsPtr);
// bounds: [min_lat, min_lon, max_lat, max_lon]
```

## JavaScript Wrapper Example

```javascript
import Carta from './build/carta.js';

class CartaTileGenerator {
  constructor() {
    this.module = null;
    this.context = null;
  }

  async init() {
    this.module = await Carta();
  }

  loadPBF(arrayBuffer) {
    const data = new Uint8Array(arrayBuffer);
    const ptr = this.module._wasm_malloc(data.length);
    this.module.HEAPU8.set(data, ptr);
    this.context = this.module._wasm_load_pbf_memory(ptr, data.length);
    this.module._wasm_free(ptr);
    return this.context !== 0;
  }

  getStats() {
    return {
      nodeCount: this.module._wasm_context_node_count(this.context),
      wayCount: this.module._wasm_context_way_count(this.context),
      bbox: this._getBBox()
    };
  }

  _getBBox() {
    const ptr = this.module._wasm_malloc(4 * 8);
    this.module._wasm_context_bbox(this.context, ptr);
    const bbox = {
      minLat: this.module.HEAPF64[ptr >> 3],
      minLon: this.module.HEAPF64[(ptr >> 3) + 1],
      maxLat: this.module.HEAPF64[(ptr >> 3) + 2],
      maxLon: this.module.HEAPF64[(ptr >> 3) + 3]
    };
    this.module._wasm_free(ptr);
    return bbox;
  }

  generateMVT(z, x, y) {
    const bufferSize = 1024 * 1024;
    const bufferPtr = this.module._wasm_malloc(bufferSize);

    const size = this.module._wasm_generate_mvt(
      this.context, z, x, y, bufferPtr, bufferSize
    );

    if (size === 0) {
      this.module._wasm_free(bufferPtr);
      return null;
    }

    const mvt = new Uint8Array(size);
    mvt.set(this.module.HEAPU8.subarray(bufferPtr, bufferPtr + size));
    this.module._wasm_free(bufferPtr);
    return mvt;
  }

  generatePNG(z, x, y, tileSize = 512) {
    const bufferSize = tileSize * tileSize * 4 + 1024;  // RGBA + overhead
    const bufferPtr = this.module._wasm_malloc(bufferSize);

    const size = this.module._wasm_generate_png(
      this.context, z, x, y, tileSize, bufferPtr, bufferSize
    );

    if (size === 0) {
      this.module._wasm_free(bufferPtr);
      return null;
    }

    const png = new Uint8Array(size);
    png.set(this.module.HEAPU8.subarray(bufferPtr, bufferPtr + size));
    this.module._wasm_free(bufferPtr);
    return png;
  }

  getTileBounds(z, x, y) {
    const ptr = this.module._wasm_malloc(4 * 8);
    this.module._wasm_tile_bounds(z, x, y, ptr);
    const bounds = {
      minLat: this.module.HEAPF64[ptr >> 3],
      minLon: this.module.HEAPF64[(ptr >> 3) + 1],
      maxLat: this.module.HEAPF64[(ptr >> 3) + 2],
      maxLon: this.module.HEAPF64[(ptr >> 3) + 3]
    };
    this.module._wasm_free(ptr);
    return bounds;
  }

  destroy() {
    if (this.context) {
      this.module._wasm_context_free(this.context);
      this.context = null;
    }
  }
}
```

## Notes

- PBF files can be loaded directly (no pre-processing needed)
- Large PBF files require significant memory (~2x file size)
- MVT tiles follow Mapbox Vector Tile spec
- PNG tiles are standard 8-bit RGBA
- Memory is managed manually - always free allocated pointers
