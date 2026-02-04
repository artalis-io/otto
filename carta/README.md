# Carta - Zero-Dependency Map Tile Generator

**C**ompact **A**gile **R**endering for **T**ile **A**rchives

A zero-dependency map tile generator that creates vector tiles (MVT) and raster tiles (PNG) from OpenStreetMap PBF files. Designed for WASM deployment alongside the FuelWise platform.

## Features

- **Vector Tiles**: Mapbox Vector Tile (MVT) format for efficient web rendering
- **Raster Tiles**: PNG tiles with configurable styling
- **Zero Dependencies**: Only standard C library + vendored miniz
- **WASM Compatible**: Works in browser via Emscripten
- **Memory Efficient**: Incremental tile generation without loading entire dataset

## Quick Start

```bash
# Build library and tests
make all

# Run tests
make test

# Generate tiles from a PBF file
./carta_cli hungary.osm.pbf --zoom 14 --output tiles/
```

## Usage

### C API

```c
#include "carta.h"

int main(void) {
    /* Load OSM data */
    CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");

    /* Generate vector tile (MVT) */
    CTTileCoord tile = {14, 9128, 5765};
    uint8_t buffer[1024 * 1024];
    size_t size = ct_generate_mvt(ctx, tile, buffer, sizeof(buffer));

    /* Generate raster tile (PNG) */
    CTStyle style;
    ct_default_style(&style);
    size = ct_generate_png(ctx, tile, &style, buffer, sizeof(buffer));

    ct_free_pbf_context(ctx);
    return 0;
}
```

### WebAssembly

```javascript
const carta = await CartaModule();
const ctx = carta.loadPBF(pbfArrayBuffer);

// Generate tiles on demand
const mvtData = carta.generateMVT(ctx, 14, 9128, 5765);
const pngData = carta.generatePNG(ctx, 14, 9128, 5765);
```

## Building

### Requirements

- C11 compiler (GCC, Clang, MSVC)
- Make (optional)
- Emscripten (for WASM build)

### Build Commands

```bash
make all       # Build library and tests
make lib       # Build libcarta.a only
make test      # Run test suite
make wasm      # Build WebAssembly (requires Emscripten)
make debug     # Debug build with symbols
make clean     # Remove build artifacts
```

## API Reference

### High-Level Functions

```c
/* Load OSM data from PBF file */
CTPBFContext *ct_load_pbf(const char *filename);

/* Generate vector tile */
size_t ct_generate_mvt(const CTPBFContext *ctx, CTTileCoord tile,
                       uint8_t *buffer, size_t capacity);

/* Generate raster tile */
size_t ct_generate_png(const CTPBFContext *ctx, CTTileCoord tile,
                       const CTStyle *style,
                       uint8_t *buffer, size_t capacity);

/* Cleanup */
void ct_free_pbf_context(CTPBFContext *ctx);
```

### Tile Coordinates

```c
typedef struct {
    int z;  /* Zoom level (0-22) */
    int x;  /* X coordinate */
    int y;  /* Y coordinate */
} CTTileCoord;

/* Get tile bounds in lat/lon */
CTBBox ct_tile_bounds(CTTileCoord tile);

/* Convert lat/lon to tile coordinates */
void ct_latlon_to_tile(double lat, double lon, int zoom,
                       int *tile_x, int *tile_y);
```

### Styling (Raster)

```c
typedef struct {
    uint32_t road_colors[8];   /* By road type */
    uint32_t water_color;
    uint32_t land_color;
    uint32_t building_color;
    float road_widths[8];      /* Pixels at z=14 */
} CTStyle;

void ct_default_style(CTStyle *style);
```

## Tile Formats

### MVT (Vector)

- Mapbox Vector Tile Specification 2.1
- Extent: 4096 units per tile
- Layers: roads, water, buildings, landuse
- Compressed with gzip (optional)

### PNG (Raster)

- 256x256 or 512x512 pixels
- RGBA color
- DEFLATE compression

## Supported OSM Features

| Layer | OSM Tags |
|-------|----------|
| roads | highway=* |
| water | natural=water, waterway=* |
| buildings | building=* |
| landuse | landuse=*, natural=* |
| railways | railway=* |

## Performance

| Operation | Time | Notes |
|-----------|------|-------|
| PBF parse (Hungary) | ~8s | 294 MB file |
| Index load (Hungary) | <1s | Pre-built binary index |
| MVT tile (z14) | ~5ms | Single tile |
| PNG tile (z14) | ~30ms | 256x256 |

## Production Deployment

For production, pre-build a binary index from the PBF file for instant startup:

```bash
# Build index (one-time, offline)
./carta-tile-server --save-index map.idx map.osm.pbf

# Run from index (production)
./carta-tile-server map.idx
```

### Binary Index Contents

The `.idx` file contains everything needed for tile serving:

| Pre-computed | Description |
|--------------|-------------|
| Ways + Coords | All geometries in fixed-point format |
| R-Tree Index | Hilbert-packed spatial index |
| Labeled Points | Cities, towns with population/priority |
| LOD Metadata | area_sqm, length_m, min_zoom per feature |
| String Pool | Deduplicated feature names |

**Not pre-computed** (computed on-the-fly):
- Simplified geometries per zoom (19x storage cost)
- Per-tile geometry clips (millions of tiles)
- Rendered tiles (use Apex for tile pyramids)

## Memory Management

Carta uses growable data structures that scale with input size:

| Region | PBF Size | Peak RAM |
|--------|----------|----------|
| Monaco | 700 KB | ~50 MB |
| Hungary | 294 MB | ~3-4 GB |
| Germany | 3.5 GB | ~35-50 GB |

### Configuration

```bash
# Set memory limit (prevents OOM, fails gracefully)
export CARTA_MEMORY_LIMIT=4G

# Initial coordinate pool size (0 = auto)
export CARTA_INITIAL_COORDS=100M

# Parse arena size
export CARTA_ARENA_SIZE=128M
```

### Programmatic Configuration

```c
CTPBFConfig config;
ct_pbf_config_init(&config);
config.memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB limit

CTPBFContext *ctx = ct_pbf_context_create_with_config(&config);
```

## Known Limitations

| Limitation | Value | Notes |
|------------|-------|-------|
| Max nodes | ~18 quintillion | `size_t` index (64-bit) |
| Max zoom level | 30 | Overflow prevention |
| Max tiles per query | 10 million | DoS protection |
| RAM requirement | ~10-15x PBF size | Peak during PBF parsing |
| Index file size | ~1.7x PBF size | Optimized for mmap |

## Integration with FuelWise

Carta can provide custom map tiles for the FuelWise UI:

```javascript
// In ui/src/components/MapView.tsx
import { cartaModule } from '../services/carta';

// Custom tile source
map.addSource('fuel-map', {
    type: 'vector',
    tiles: async (z, x, y) => carta.generateMVT(ctx, z, x, y)
});
```

## Directory Structure

```
carta/
├── include/         # Public headers
│   ├── carta.h      # Unified API
│   ├── ct_types.h   # Data structures
│   ├── ct_tile.h    # Tile math
│   ├── ct_mvt.h     # MVT encoding
│   └── ct_render.h  # Rasterization
├── src/             # Implementation
├── vendor/          # miniz (shared with velo)
├── tests/           # Test suite
└── benchmarks/      # Performance tests
```

## License

MIT License - see LICENSE file.

## See Also

- [velo](../velo/) - OSM routing engine
- [shared](../shared/) - Shared protobuf/inflate/PBF parsing library (used by both velo and carta)
- [fuelwise](../fuelwise/) - Refueling optimization
- [Mapbox Vector Tile Spec](https://github.com/mapbox/vector-tile-spec)
