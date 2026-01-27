# Carta - Map Tile Generator Implementation Plan

**C**ompact **A**gile **R**endering for **T**ile **A**rchives

A zero-dependency map tile generator for OSM PBF files, designed for WASM deployment.

## Overview

Carta generates both vector tiles (MVT format) and raster tiles (PNG) from OpenStreetMap PBF files. It follows the project's C-first, zero-dependency architecture and shares infrastructure with velo (PBF parsing, miniz).

## Goals

1. **Vector Tiles**: Generate Mapbox Vector Tiles (MVT) for efficient web rendering
2. **Raster Tiles**: Generate PNG tiles for fallback/preview rendering
3. **Zero Dependencies**: Only standard C library + vendored miniz
4. **WASM Compatible**: Works in browser via Emscripten
5. **Incremental**: Support tile-by-tile generation (don't load entire world)
6. **Integration**: Use with FuelWise UI for offline/custom map tiles

## Architecture

```
carta/
├── include/
│   ├── carta.h          # Unified public API
│   ├── ct_types.h       # Core data structures
│   ├── ct_pbf.h         # PBF parsing (shared concepts with velo)
│   ├── ct_tile.h        # Tile coordinate math
│   ├── ct_mvt.h         # MVT encoding
│   ├── ct_render.h      # Raster rendering
│   └── ct_png.h         # PNG encoding
├── src/
│   ├── carta.c          # Main API implementation
│   ├── ct_pbf.c         # PBF parsing for map features
│   ├── ct_tile.c        # Web Mercator projection, tile coords
│   ├── ct_mvt.c         # MVT protobuf encoding
│   ├── ct_render.c      # Software rasterizer
│   ├── ct_png.c         # PNG encoder using miniz
│   └── ct_style.c       # Basic styling (colors, line widths)
├── vendor/              # Shared: symlink to ../velo/vendor or copy
├── tests/
│   └── test_carta.c
├── benchmarks/
│   └── bench_carta.c
├── Makefile
├── README.md
└── CLAUDE.md
```

## Data Structures

### Core Types

```c
/* Tile coordinates (Web Mercator) */
typedef struct {
    int z;          /* Zoom level (0-22) */
    int x;          /* Tile X coordinate */
    int y;          /* Tile Y coordinate */
} CTTileCoord;

/* Bounding box in lat/lon */
typedef struct {
    double min_lat, min_lon;
    double max_lat, max_lon;
} CTBBox;

/* Geographic coordinate */
typedef struct {
    double lat;
    double lon;
} CTCoord;

/* Point in tile coordinates (0-4096 for MVT) */
typedef struct {
    int32_t x;
    int32_t y;
} CTTilePoint;

/* Feature geometry types */
typedef enum {
    CT_GEOM_POINT = 1,
    CT_GEOM_LINESTRING = 2,
    CT_GEOM_POLYGON = 3
} CTGeomType;

/* Map feature (road, building, etc.) */
typedef struct {
    CTGeomType type;
    CTTilePoint *points;
    int num_points;
    int *ring_starts;    /* For polygons: start indices of rings */
    int num_rings;
    int layer;           /* Feature layer (roads, water, etc.) */
    int feature_type;    /* Specific type within layer */
} CTFeature;

/* Tile data ready for encoding */
typedef struct {
    CTFeature *features;
    size_t num_features;
    size_t capacity;
    CTTileCoord coord;
} CTTile;

/* Style configuration */
typedef struct {
    uint32_t road_colors[8];     /* By road type */
    uint32_t water_color;
    uint32_t land_color;
    uint32_t building_color;
    float road_widths[8];        /* By road type, in pixels at z=14 */
} CTStyle;

/* Rendering context */
typedef struct {
    uint8_t *pixels;             /* RGBA buffer */
    int width;
    int height;
    CTStyle style;
} CTRenderContext;
```

### PBF Parsing Context

```c
/* OSM feature types we care about */
typedef enum {
    CT_OSM_HIGHWAY,
    CT_OSM_WATER,
    CT_OSM_BUILDING,
    CT_OSM_LANDUSE,
    CT_OSM_NATURAL,
    CT_OSM_RAILWAY,
    CT_OSM_BOUNDARY
} CTOSMFeatureClass;

/* Parsed OSM way with geometry */
typedef struct {
    int64_t id;
    CTCoord *coords;
    int num_coords;
    CTOSMFeatureClass feature_class;
    int feature_type;            /* Subtype within class */
    int is_area;                 /* Closed polygon? */
} CTOSMWay;

/* PBF parsing context */
typedef struct {
    /* Node storage (for resolving way references) */
    struct {
        int64_t *ids;
        CTCoord *coords;
        size_t count;
        size_t capacity;
    } nodes;

    /* Parsed ways */
    CTOSMWay *ways;
    size_t num_ways;
    size_t ways_capacity;

    /* Spatial index for fast tile queries */
    struct CTRTree *rtree;

    /* Bounding box of loaded data */
    CTBBox bbox;
} CTPBFContext;
```

## Key Algorithms

### 1. Web Mercator Projection

```c
/* Lat/lon to tile coordinates */
void ct_latlon_to_tile(double lat, double lon, int zoom,
                       int *tile_x, int *tile_y);

/* Lat/lon to pixel within tile (0-4096 for MVT, 0-256 for raster) */
void ct_latlon_to_tile_pixel(double lat, double lon, CTTileCoord tile,
                             int extent, int *px, int *py);

/* Tile bounds in lat/lon */
CTBBox ct_tile_bounds(CTTileCoord tile);
```

### 2. MVT Encoding

MVT uses Google Protobuf with specific structure:
- Layer contains features
- Features have geometry encoded as commands (MoveTo, LineTo, ClosePath)
- Coordinates are delta-encoded integers

```c
/* Encode tile to MVT format */
size_t ct_encode_mvt(const CTTile *tile, uint8_t *buffer, size_t capacity);

/* Geometry command encoding */
void ct_mvt_encode_geometry(const CTFeature *feature,
                            uint8_t *buffer, size_t *offset);
```

### 3. Software Rasterizer

Simple scanline rasterizer for:
- Anti-aliased lines (Xiaolin Wu's algorithm)
- Polygon filling (scanline with edge table)

```c
/* Initialize render context */
CTRenderContext *ct_render_create(int width, int height);

/* Render tile to pixels */
void ct_render_tile(CTRenderContext *ctx, const CTTile *tile);

/* Draw primitives */
void ct_render_line(CTRenderContext *ctx, int x0, int y0, int x1, int y1,
                    uint32_t color, float width);
void ct_render_polygon(CTRenderContext *ctx, const CTTilePoint *points,
                       int num_points, uint32_t fill_color);
```

### 4. PNG Encoding

Using miniz for DEFLATE compression:

```c
/* Encode RGBA pixels to PNG */
size_t ct_encode_png(const uint8_t *pixels, int width, int height,
                     uint8_t *buffer, size_t capacity);
```

## API Design

### High-Level API

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

/* Batch generation */
typedef void (*CTTileCallback)(CTTileCoord tile, const uint8_t *data,
                               size_t size, void *user_data);
void ct_generate_tiles(const CTPBFContext *ctx, CTBBox bounds,
                       int min_zoom, int max_zoom,
                       int vector, CTTileCallback callback, void *user_data);

/* Cleanup */
void ct_free_pbf_context(CTPBFContext *ctx);
```

### Low-Level API

```c
/* Tile coordinate utilities */
CTBBox ct_tile_bounds(CTTileCoord tile);
int ct_tiles_for_bbox(CTBBox bbox, int zoom, CTTileCoord **tiles);

/* Manual tile building */
CTTile *ct_tile_create(CTTileCoord coord);
void ct_tile_add_feature(CTTile *tile, const CTFeature *feature);
void ct_tile_free(CTTile *tile);

/* Feature extraction */
int ct_extract_features(const CTPBFContext *ctx, CTTileCoord tile,
                        CTFeature **features, size_t *count);
```

## Implementation Phases

### Phase 1: Core Infrastructure
- [ ] Directory structure and build system
- [ ] ct_types.h with all data structures
- [ ] ct_tile.c: Web Mercator math and tile coordinates
- [ ] Basic test harness

### Phase 2: PBF Parsing
- [ ] ct_pbf.c: Parse nodes, ways with relevant tags
- [ ] Filter for map-relevant features (roads, water, buildings)
- [ ] Node ID -> coordinate lookup (hash map)
- [ ] Build way geometries from node references

### Phase 3: Vector Tiles (MVT)
- [ ] ct_mvt.c: Protobuf encoding (minimal, write-only)
- [ ] Geometry command encoding (MoveTo, LineTo, ClosePath)
- [ ] Delta coordinate encoding
- [ ] Layer organization (roads, water, buildings)
- [ ] Feature attribute encoding

### Phase 4: Raster Tiles
- [ ] ct_render.c: Pixel buffer management
- [ ] Line drawing (Bresenham + anti-aliasing)
- [ ] Polygon filling (scanline algorithm)
- [ ] ct_style.c: Color/width configuration
- [ ] ct_png.c: PNG encoding with miniz

### Phase 5: Optimization
- [ ] R-tree spatial index for fast tile queries
- [ ] Geometry simplification for low zoom levels
- [ ] Coordinate clipping to tile bounds
- [ ] Memory pooling for feature allocation

### Phase 6: WASM & Integration
- [ ] Emscripten build configuration
- [ ] JavaScript bindings
- [ ] Integration example with FuelWise UI
- [ ] In-browser tile generation demo

## File Format Specifications

### MVT (Mapbox Vector Tile)

Based on [Mapbox Vector Tile Specification 2.1](https://github.com/mapbox/vector-tile-spec):

- Protobuf-encoded
- Tile extent: 4096 (standard)
- Coordinate range: 0-4095 within tile
- Geometry commands:
  - MoveTo: `(1 << 3) | 1`
  - LineTo: `(2 << 3) | count`
  - ClosePath: `(7 << 3) | 1`
- Coordinates are zigzag-encoded deltas

### PNG

Standard PNG with:
- RGBA color type (6)
- 8-bit depth
- DEFLATE compression via miniz
- No interlacing (simpler)

## Dependencies

| Dependency | Source | Purpose |
|------------|--------|---------|
| miniz | vendor/ (shared with velo) | DEFLATE for PNG |
| Standard C lib | Built-in | Memory, math, I/O |

## Performance Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| PBF parse (Hungary) | < 10s | Similar to velo |
| Single MVT tile | < 10ms | At z14 |
| Single PNG tile | < 50ms | 256x256 |
| Memory usage | < 500MB | For country-scale data |

## Testing Strategy

1. **Unit tests**: Coordinate math, encoding functions
2. **Integration tests**: Full PBF -> tile pipeline
3. **Visual tests**: Render sample tiles, compare to reference
4. **WASM tests**: Browser-based tile generation

## Open Questions

1. **Shared PBF code**: Copy velo's PBF parser or create shared library?
   - Recommendation: Copy with modifications for map features (different tag filtering)

2. **Tile caching**: Include built-in caching or leave to user?
   - Recommendation: Leave to user; provide efficient single-tile API

3. **Style system**: Simple hardcoded vs. configurable?
   - Recommendation: Start simple, add configurability in Phase 5

4. **Multi-threading**: OpenMP like velo?
   - Recommendation: Optional; single-threaded by default for WASM

## Usage Example

```c
#include "carta.h"

int main(void) {
    /* Load OSM data */
    CTPBFContext *ctx = ct_load_pbf("hungary.osm.pbf");

    /* Generate a single vector tile */
    CTTileCoord tile = {14, 9128, 5765};  /* Budapest area */
    uint8_t mvt_buffer[1024 * 1024];
    size_t mvt_size = ct_generate_mvt(ctx, tile, mvt_buffer, sizeof(mvt_buffer));

    /* Generate a raster tile */
    CTStyle style;
    ct_default_style(&style);
    uint8_t png_buffer[256 * 1024];
    size_t png_size = ct_generate_png(ctx, tile, &style, png_buffer, sizeof(png_buffer));

    /* Save to files */
    FILE *f = fopen("tile.mvt", "wb");
    fwrite(mvt_buffer, 1, mvt_size, f);
    fclose(f);

    f = fopen("tile.png", "wb");
    fwrite(png_buffer, 1, png_size, f);
    fclose(f);

    ct_free_pbf_context(ctx);
    return 0;
}
```

## WASM Usage

```javascript
// Load WASM module
const carta = await CartaModule();

// Load PBF data (from fetch or file input)
const pbfData = await fetch('map.osm.pbf').then(r => r.arrayBuffer());
const ctx = carta.loadPBF(pbfData);

// Generate tiles on demand
function getTile(z, x, y, format) {
    if (format === 'mvt') {
        return carta.generateMVT(ctx, z, x, y);
    } else {
        return carta.generatePNG(ctx, z, x, y);
    }
}

// Use with map library (e.g., MapLibre GL JS)
map.addSource('custom', {
    type: 'vector',
    tiles: ['custom://{z}/{x}/{y}.mvt'],
    // Custom tile loading using carta
});
```

## Next Steps

1. Review and approve this plan
2. Create directory structure and Makefile
3. Implement Phase 1 (core infrastructure)
4. Iteratively implement remaining phases
