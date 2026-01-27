# Claude Code Instructions for Carta

## Project Overview

Carta is a zero-dependency map tile generator that creates vector tiles (MVT) and raster tiles (PNG) from OpenStreetMap PBF files. It's designed for WASM deployment alongside FuelWise.

## Quick Start

```bash
# Build and test
make all && make test

# Run benchmarks
make bench
```

## Directory Structure

```
carta/
├── include/          # Public headers
│   ├── carta.h       # Unified API
│   ├── ct_types.h    # Data structures
│   ├── ct_tile.h     # Web Mercator math
│   ├── ct_mvt.h      # MVT encoding
│   ├── ct_render.h   # Rasterization
│   └── ct_png.h      # PNG encoding
├── src/              # Implementation
│   ├── carta.c       # Main API
│   ├── ct_pbf.c      # PBF parsing
│   ├── ct_tile.c     # Coordinate transforms
│   ├── ct_mvt.c      # MVT protobuf
│   ├── ct_render.c   # Software renderer
│   ├── ct_png.c      # PNG encoder
│   └── ct_style.c    # Styling
├── vendor/           # miniz (shared with velo)
├── tests/            # Test suite
└── benchmarks/       # Performance tests
```

## Key Files

| File | Purpose |
|------|---------|
| `ct_types.h` | All data structures |
| `ct_tile.c` | Web Mercator projection, tile coords |
| `ct_mvt.c` | Vector tile encoding |
| `ct_render.c` | Raster tile drawing |
| `ct_pbf.c` | OSM PBF parsing |

## Build Commands

```bash
make all      # Build library + tests
make lib      # Build libcarta.a only
make test     # Run test suite
make bench    # Build and run benchmarks
make debug    # Debug build with symbols
make wasm     # WebAssembly (needs Emscripten)
make clean    # Remove build artifacts
```

## Architecture

### Data Flow

```
PBF File → ct_pbf.c → CTPBFContext → ct_tile.c → CTTile → ct_mvt.c → MVT bytes
                                                        ↘ ct_render.c → ct_png.c → PNG bytes
```

### Web Mercator Projection

Key formulas in `ct_tile.c`:

```c
// Lat/lon to tile coordinates
tile_x = floor((lon + 180) / 360 * (1 << zoom))
tile_y = floor((1 - log(tan(lat) + 1/cos(lat)) / PI) / 2 * (1 << zoom))

// Tile to lat/lon bounds
lon = tile_x / (1 << zoom) * 360 - 180
lat = atan(sinh(PI * (1 - 2 * tile_y / (1 << zoom)))) * 180 / PI
```

### MVT Encoding

MVT uses protobuf with geometry commands:
- MoveTo (1): Start new path
- LineTo (2): Continue path
- ClosePath (7): Close polygon

Coordinates are delta-encoded and zigzag-encoded.

### Raster Rendering

Software rasterizer in `ct_render.c`:
- Xiaolin Wu anti-aliased lines
- Scanline polygon fill
- Per-pixel alpha blending

## Common Tasks

### Adding a new feature layer

1. Add layer enum in `ct_types.h`
2. Update tag filtering in `ct_pbf.c`
3. Add layer encoding in `ct_mvt.c`
4. Add styling in `ct_style.c`
5. Add rendering in `ct_render.c`

### Modifying tile styling

1. Edit default colors/widths in `ct_style.c`
2. Update `ct_default_style()` function
3. Run visual tests

### Optimizing performance

1. Check R-tree queries in `ct_pbf.c`
2. Profile geometry clipping
3. Consider geometry simplification at low zoom

## Testing

```bash
# Run all tests
make test

# Visual test - generate sample tiles
./test_carta --visual

# Benchmark tile generation
./bench_carta map.osm.pbf
```

## Memory Management

```c
// Always free contexts
CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");
// ... use ctx ...
ct_free_pbf_context(ctx);

// Tiles are value types (stack or caller-managed)
CTTile tile;
ct_tile_init(&tile, coord);
// ... add features ...
ct_tile_clear(&tile);
```

## Code Style

- 4-space indentation
- `ct_` prefix for all public functions
- `CT` prefix for all public types
- Comments for algorithm steps

## Dependencies

- **External**: None
- **Vendored**: miniz (public domain zlib)
- **Shared**: Can use velo's vendor/miniz
- **Standard Library**: stdio, stdlib, string, math

## Performance Notes

- R-tree spatial index is critical for large datasets
- Geometry simplification saves bandwidth at low zoom
- MVT encoding is I/O bound (compression)
- PNG encoding is CPU bound (rasterization + DEFLATE)

## WASM Considerations

- No mmap (use malloc + fread)
- No threads (single-threaded)
- Memory limit considerations for large PBF files
- Export minimal API surface
