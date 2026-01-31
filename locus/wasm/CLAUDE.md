# Locus WASM - Claude Instructions

## Overview

Locus WASM compiles the C geocoding library to WebAssembly for browser use. It wraps the Locus library with browser-friendly entry points for search, autocomplete, and reverse geocoding.

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
| `src/locus_wasm.c` | WASM entry points |
| `Makefile` | Emscripten build |
| `build/locus.js` | Generated JS loader |
| `build/locus.wasm` | Generated WASM binary |

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

### Index Loading
```javascript
// Create index
const indexPtr = Module._wasm_index_create();

// Load PBF data from ArrayBuffer
const data = new Uint8Array(pbfArrayBuffer);
const dataPtr = Module._wasm_malloc(data.length);
Module.HEAPU8.set(data, dataPtr);
const status = Module._wasm_index_load_pbf_memory(indexPtr, dataPtr, data.length);
Module._wasm_free(dataPtr);

if (status === 0) {
  const entityCount = Module._wasm_index_entity_count(indexPtr);
  console.log(`Loaded ${entityCount} entities`);
}

// Free index when done
Module._wasm_index_free(indexPtr);
```

### Search
```javascript
// Allocate string in WASM memory
function allocString(str) {
  const len = Module.lengthBytesUTF8(str) + 1;
  const ptr = Module._wasm_malloc(len);
  Module.stringToUTF8(str, ptr, len);
  return ptr;
}

// Forward geocoding search
const queryPtr = allocString("Budapest");
const resultPtr = Module._wasm_search(indexPtr, queryPtr, 10, 1);  // limit=10, fuzzy=1
Module._wasm_free(queryPtr);

if (resultPtr) {
  const count = Module._wasm_result_count(resultPtr);
  for (let i = 0; i < count; i++) {
    const namePtr = Module._wasm_result_get_name(resultPtr, i);
    const name = Module.UTF8ToString(namePtr);
    const lat = Module._wasm_result_get_lat(resultPtr, i);
    const lon = Module._wasm_result_get_lon(resultPtr, i);
    const score = Module._wasm_result_get_score(resultPtr, i);
    console.log(`${name}: ${lat}, ${lon} (score: ${score})`);
  }
  Module._wasm_result_free(resultPtr);
}
```

### Autocomplete
```javascript
const prefixPtr = allocString("Bud");
const resultPtr = Module._wasm_autocomplete(indexPtr, prefixPtr, 10);
Module._wasm_free(prefixPtr);

if (resultPtr) {
  const count = Module._wasm_result_count(resultPtr);
  // ... process results same as search
  Module._wasm_result_free(resultPtr);
}
```

### Reverse Geocoding
```javascript
// Find what's near a coordinate
const resultPtr = Module._wasm_reverse(indexPtr, 47.4979, 19.0402, 100.0);  // lat, lon, radius_m

if (resultPtr) {
  const count = Module._wasm_result_count(resultPtr);
  for (let i = 0; i < count; i++) {
    const namePtr = Module._wasm_result_get_name(resultPtr, i);
    if (namePtr) {
      console.log(Module.UTF8ToString(namePtr));
    }
  }
  Module._wasm_result_free(resultPtr);
}
```

## JavaScript Wrapper Example

```javascript
import Locus from './build/locus.js';

class LocusGeocoder {
  constructor() {
    this.module = null;
    this.index = null;
  }

  async init() {
    this.module = await Locus();
  }

  async loadPBF(arrayBuffer) {
    this.index = this.module._wasm_index_create();

    const data = new Uint8Array(arrayBuffer);
    const ptr = this.module._wasm_malloc(data.length);
    this.module.HEAPU8.set(data, ptr);

    const status = this.module._wasm_index_load_pbf_memory(this.index, ptr, data.length);
    this.module._wasm_free(ptr);

    return status === 0;
  }

  _allocString(str) {
    const len = this.module.lengthBytesUTF8(str) + 1;
    const ptr = this.module._wasm_malloc(len);
    this.module.stringToUTF8(str, ptr, len);
    return ptr;
  }

  search(query, limit = 10, fuzzy = true) {
    const queryPtr = this._allocString(query);
    const resultPtr = this.module._wasm_search(this.index, queryPtr, limit, fuzzy ? 1 : 0);
    this.module._wasm_free(queryPtr);

    if (!resultPtr) return [];
    return this._extractResults(resultPtr);
  }

  autocomplete(prefix, limit = 10) {
    const prefixPtr = this._allocString(prefix);
    const resultPtr = this.module._wasm_autocomplete(this.index, prefixPtr, limit);
    this.module._wasm_free(prefixPtr);

    if (!resultPtr) return [];
    return this._extractResults(resultPtr);
  }

  reverse(lat, lon, radius = 100) {
    const resultPtr = this.module._wasm_reverse(this.index, lat, lon, radius);
    if (!resultPtr) return [];
    return this._extractResults(resultPtr);
  }

  _extractResults(resultPtr) {
    const results = [];
    const count = this.module._wasm_result_count(resultPtr);

    for (let i = 0; i < count; i++) {
      const namePtr = this.module._wasm_result_get_name(resultPtr, i);
      results.push({
        id: this.module._wasm_result_get_id(resultPtr, i),
        name: namePtr ? this.module.UTF8ToString(namePtr) : null,
        lat: this.module._wasm_result_get_lat(resultPtr, i),
        lon: this.module._wasm_result_get_lon(resultPtr, i),
        score: this.module._wasm_result_get_score(resultPtr, i)
      });
    }

    this.module._wasm_result_free(resultPtr);
    return results;
  }

  getStats() {
    return {
      entityCount: this.module._wasm_index_entity_count(this.index),
      memoryUsage: this.module._wasm_index_memory_usage(this.index)
    };
  }

  destroy() {
    if (this.index) {
      this.module._wasm_index_free(this.index);
      this.index = null;
    }
  }
}

// Usage
const geocoder = new LocusGeocoder();
await geocoder.init();

const response = await fetch('monaco.osm.pbf');
await geocoder.loadPBF(await response.arrayBuffer());

const results = geocoder.search('Monte Carlo');
console.log(results);

const nearby = geocoder.reverse(43.7384, 7.4246);
console.log(nearby);

geocoder.destroy();
```

## Notes

- PBF files are loaded directly (no pre-processing needed)
- Memory is managed manually - always free allocated pointers and results
- String pointers returned by `_wasm_result_get_name` point to internal memory and should not be freed
- Coordinates are in lat/lon order
- Initial memory is set to 64MB and can grow as needed
