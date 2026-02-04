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
│   ├── ct_pbf.c      # PBF parsing (uses shared library)
│   ├── ct_tile.c     # Coordinate transforms
│   ├── ct_mvt.c      # MVT protobuf (uses shared sh_pb_write_*)
│   ├── ct_render.c   # Software renderer
│   ├── ct_png.c      # PNG encoder (uses shared sh_deflate)
│   ├── ct_style.c    # Styling
│   ├── ct_lod.c      # Level-of-detail filtering
│   └── ct_simplify.c # Geometry simplification
├── api/              # Tile server REST API
├── ../shared/        # Shared library (protobuf, inflate, PBF parsing)
├── ../vendor/        # Third-party (miniz)
├── tests/            # Test suite
└── benchmarks/       # Performance tests
```

**Note:** Protobuf encoding/decoding and zlib compression are provided by the shared library (`sh_protobuf.h`, `sh_inflate.h`, `sh_pbf.h`).

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

### Overview

Carta uses a hybrid memory strategy optimized for large-scale OSM data:

| Structure | Strategy | Behavior |
|-----------|----------|----------|
| `node_map` | Growable hash map | Doubles at 75% load |
| `way_map` | Growable hash map | Doubles at 75% load |
| `coord_pool` | Growable pool | Doubles when full |
| `parse_arena` | Fixed arena | Reset per PrimitiveBlock |

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CARTA_MEMORY_LIMIT` | 0 (unlimited) | Max memory usage (supports K/M/G suffix) |
| `CARTA_INITIAL_COORDS` | auto | Initial coordinate pool size |
| `CARTA_ARENA_SIZE` | 128M | Parse arena size |

**Examples:**
```bash
# Limit to 4GB memory
export CARTA_MEMORY_LIMIT=4G

# Set initial coord pool to 100M coordinates
export CARTA_INITIAL_COORDS=100M
```

### Programmatic Configuration

```c
CTPBFConfig config;
ct_pbf_config_init(&config);
config.memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB
config.arena_size = 256 * 1024 * 1024;            // 256MB arena

CTPBFContext *ctx = ct_pbf_context_create_with_config(&config);
// ... use ctx ...
ct_pbf_context_free(ctx);
```

### Basic Usage

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

### Memory Estimates by Region

| Region | PBF Size | Peak RAM | Notes |
|--------|----------|----------|-------|
| Monaco | 700 KB | ~50 MB | Good for testing |
| Hungary | 294 MB | ~3-4 GB | Country-scale |
| Germany | 3.5 GB | ~35-50 GB | Large country |
| Europe | 25 GB | ~250-350 GB | Continental |
| Planet | 70 GB | ~700 GB-1 TB | Global |

Rule of thumb: **10-15x PBF file size** for peak memory usage.

## Code Style

- 4-space indentation
- `ct_` prefix for all public functions
- `CT` prefix for all public types
- Comments for algorithm steps

## Dependencies

- **External**: None
- **Shared**: ../shared (protobuf, inflate, PBF parsing, geo utilities)
- **Vendored**: ../vendor/miniz (public domain zlib, used via shared library)
- **Standard Library**: stdio, stdlib, string, math

## Performance Notes

- R-tree spatial index is critical for large datasets
- Geometry simplification saves bandwidth at low zoom
- MVT encoding is I/O bound (compression)
- PNG encoding is CPU bound (rasterization + DEFLATE)

## Known Limitations

| Limitation | Value | Notes |
|------------|-------|-------|
| Max nodes | ~4 billion | `uint32_t` index |
| Max zoom | 30 | `1 << z` overflow prevention |
| Max tiles per bbox query | 10 million | DoS protection |
| Thread-local render cache | Not freed on thread exit | Minor leak in multi-threaded apps |

### Workarounds for Large Datasets

For continent/planet-scale data:
1. **Use regional extracts** - Geofabrik provides country/region PBFs
2. **Pre-generate tiles** - Use Apex (planned) for tile pyramids
3. **Set memory limit** - Graceful failure vs OOM kill
4. **Increase system RAM** - Linear scaling with data size

## WASM Considerations

- No mmap (use malloc + fread)
- No threads (single-threaded)
- Memory limit considerations for large PBF files
- Export minimal API surface
- WASM linear memory provides bounds checking (defense in depth)
