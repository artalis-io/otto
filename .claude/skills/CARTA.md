# Carta Map Tile Generator - Usage Guide

## Overview

Carta is a zero-dependency map tile generator that creates vector tiles (MVT) and raster tiles (PNG) from OpenStreetMap PBF files. It's designed for WASM deployment and integrates with Leaflet/Google Maps.

## Quick Start

```bash
# Build and test
cd carta
make all && make test

# Run benchmarks
make bench
```

## Building Carta

### Basic Build

```bash
cd carta
make all       # Build library + tests
make lib       # Build libcarta.a only
make test      # Run test suite (33 tests)
make bench     # Build and run benchmarks
make debug     # Debug build with symbols
make wasm      # WebAssembly (needs Emscripten)
make clean     # Remove build artifacts
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
├── api/              # Tile server REST API
├── ui/               # Tile viewer React app
├── tests/            # Test suite
└── benchmarks/       # Performance tests
```

## Using Carta in Your Code

### Generate a PNG Tile

```c
#include "carta.h"

int main() {
    // Load PBF data
    CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");
    if (!ctx) {
        fprintf(stderr, "Failed to load PBF\n");
        return 1;
    }

    // Define tile coordinates (z/x/y)
    CTTileCoord coord = {14, 9058, 5729};  // Budapest area

    // Generate PNG tile
    CTTile tile;
    ct_tile_init(&tile, coord);
    ct_generate_tile(ctx, &tile);

    // Render to PNG
    uint8_t *png_data;
    size_t png_size;
    ct_render_png(&tile, 512, &png_data, &png_size);

    // Save to file
    FILE *f = fopen("tile.png", "wb");
    fwrite(png_data, 1, png_size, f);
    fclose(f);

    free(png_data);
    ct_tile_clear(&tile);
    ct_free_pbf_context(ctx);
    return 0;
}
```

### Generate an MVT Tile

```c
#include "carta.h"

int main() {
    CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");

    CTTileCoord coord = {14, 9058, 5729};
    CTTile tile;
    ct_tile_init(&tile, coord);
    ct_generate_tile(ctx, &tile);

    // Encode to MVT (Mapbox Vector Tile)
    uint8_t *mvt_data;
    size_t mvt_size;
    ct_encode_mvt(&tile, &mvt_data, &mvt_size);

    // Save to file
    FILE *f = fopen("tile.mvt", "wb");
    fwrite(mvt_data, 1, mvt_size, f);
    fclose(f);

    free(mvt_data);
    ct_tile_clear(&tile);
    ct_free_pbf_context(ctx);
    return 0;
}
```

### Custom Styling

```c
#include "carta.h"

int main() {
    CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");

    // Create custom style
    CTStyle style;
    ct_default_style(&style);

    // Modify colors
    style.road_color = 0xFF0000FF;      // Red roads (RGBA)
    style.water_color = 0x0066CCFF;     // Blue water
    style.building_color = 0x808080FF;  // Gray buildings

    CTTileCoord coord = {14, 9058, 5729};
    CTTile tile;
    ct_tile_init(&tile, coord);
    ct_generate_tile(ctx, &tile);

    // Render with custom style
    uint8_t *png_data;
    size_t png_size;
    ct_render_png_styled(&tile, 512, &style, &png_data, &png_size);

    // ... save file ...

    ct_free_pbf_context(ctx);
    return 0;
}
```

### Compiling Your Program

```bash
# Compile with Carta (requires shared and miniz)
gcc -O3 -I./include -I../shared/include -I../vendor/miniz \
    myprogram.c -L. -lcarta -L../shared -lshared -L../vendor/miniz -lminiz -lm \
    -o myprogram
```

## API Reference

### PBF Loading
- `ct_load_pbf(path)` - Load OSM PBF file
- `ct_free_pbf_context(ctx)` - Free PBF context

### Tile Generation
- `ct_tile_init(tile, coord)` - Initialize tile structure
- `ct_generate_tile(ctx, tile)` - Generate tile features from PBF
- `ct_tile_clear(tile)` - Free tile resources

### MVT Encoding
- `ct_encode_mvt(tile, data, size)` - Encode to MVT format

### PNG Rendering
- `ct_render_png(tile, size, data, len)` - Render to PNG (default style)
- `ct_render_png_styled(tile, size, style, data, len)` - Render with custom style

### Styling
- `ct_default_style(style)` - Initialize default style

### Coordinate Utilities
- `ct_latlon_to_tile(lat, lon, zoom, x, y)` - Convert lat/lon to tile coordinates
- `ct_tile_bounds(coord)` - Get bounding box for tile

## Web Mercator Projection

Key formulas used internally:

```c
// Lat/lon to tile coordinates
tile_x = floor((lon + 180) / 360 * (1 << zoom))
tile_y = floor((1 - log(tan(lat) + 1/cos(lat)) / PI) / 2 * (1 << zoom))

// Tile to lat/lon bounds
lon = tile_x / (1 << zoom) * 360 - 180
lat = atan(sinh(PI * (1 - 2 * tile_y / (1 << zoom)))) * 180 / PI
```

## MVT Format

MVT uses protobuf with geometry commands:
- **MoveTo (1)**: Start new path
- **LineTo (2)**: Continue path
- **ClosePath (7)**: Close polygon

Coordinates are delta-encoded and zigzag-encoded for efficient storage.

## Performance Notes

| Operation | Time | Notes |
|-----------|------|-------|
| PBF load (Hungary, 300MB) | ~10-15s | One-time |
| PNG tile (512x512) | ~75ms | CPU-bound |
| MVT tile | ~10-30ms | I/O-bound (compression) |

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

## Dependencies

- **External**: None
- **Vendored**: ../vendor/miniz (public domain zlib)
- **Shared**: ../shared (geo utilities)
- **Standard Library**: stdio, stdlib, string, math

## WASM Considerations

- No mmap (use malloc + fread)
- No threads (single-threaded)
- Memory limit considerations for large PBF files
- Export minimal API surface
