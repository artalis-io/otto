# Carta Tile Server Roadmap

Comprehensive development plan for the Carta map tile generator covering performance optimization, feature improvements, and label rendering.

## Status Summary (Feb 2026)

| Area | Status | Key Files |
|------|--------|-----------|
| **R-Tree Spatial Index** | ✅ Complete | `ct_rtree.c` |
| **Tile Cache** | ✅ Complete | `ct_cache.c` |
| **Clipping & Simplification** | ✅ Complete | `ct_clip.c`, `ct_simplify.c` |
| **Memory Optimization** | ✅ Complete | Reusable buffers, context pooling |
| **LOD Rules** | ✅ Complete | OSM-like zoom rules |
| **Point Labels** | ✅ Complete | Cities, towns, POIs |
| **Road Labels** | ✅ Complete | Text along paths |
| **Area Labels** | ✅ Complete | Lakes, parks |
| **Metatile Label Cache** | ✅ Complete | Cross-tile label consistency |
| **ETags & HTTP Caching** | ✅ Complete | `api/src/main.c` |
| **Road Styling (OSM Carto)** | ❌ TODO | `ct_style.c`, `ct_render.c` |
| **@2x Retina Tiles** | ❌ TODO | `api/src/main.c` |
| **MVT Labels** | ❌ TODO | Vector tile label layer |
| **Continental Sharding** | ❌ TODO | Multi-region tile serving |

**Current Performance**: ~75ms average PNG tile latency (down from ~240ms)
**Test Coverage**: 143 carta tests + 21 metatile tests = 164 total

---

## Chapter 1: Performance Optimization

### 1.1 Memory Allocation (✅ Complete)

**Problem**: 500+ malloc/free pairs per tile caused ~100-150ms overhead.

**Solution**: Pre-allocated reusable buffers in render context.

```c
typedef struct {
    uint8_t *pixels;
    int width, height, stride;
    CTStyle style;

    /* Pre-allocated buffers for rendering */
    CTTilePoint *scale_buffer;
    size_t scale_buffer_capacity;
    CTEdge *edge_buffer;
    size_t edge_buffer_capacity;
} CTRenderContext;
```

**Impact**: Eliminated 500+ malloc/free pairs per tile.

### 1.2 Polygon Sorting (✅ Complete)

**Problem**: O(n²) bubble sort in scanline algorithm caused ~80-120ms overhead.

**Solution**: Replaced with O(n) insertion sort for nearly-sorted edge lists.

```c
static void sort_edges_by_x(CTEdge *active, int num_active) {
    for (int i = 1; i < num_active; i++) {
        CTEdge key = active[i];
        int j = i - 1;
        while (j >= 0 && active[j].x > key.x) {
            active[j + 1] = active[j];
            j--;
        }
        active[j + 1] = key;
    }
}
```

### 1.3 R-Tree Spatial Index (✅ Complete)

**Problem**: O(n) linear scan of all ways for every tile request.

**Solution**: Hilbert R-Tree with Sort-Tile-Recursive packing.

```c
typedef struct CTRTreeNode {
    CTBBox bbox;
    int is_leaf;
    int count;
    union {
        struct CTRTreeNode *children[CT_RTREE_MAX_ENTRIES];
        uint32_t way_indices[CT_RTREE_MAX_ENTRIES];
    };
} CTRTreeNode;
```

**Impact**: Tile queries reduced from O(n) to O(log n + k).

### 1.4 Render Context Pooling (✅ Complete)

**Problem**: 1MB allocation per tile request.

**Solution**: Pool of reusable render contexts.

### 1.5 Zoom-Adaptive Anti-Aliasing (❌ TODO)

**Goal**: Disable expensive AA at low zoom levels (z < 12).

**Priority**: Low (5% improvement expected).

### 1.6 Additional Completed Optimizations

| Optimization | Impact | Location |
|-------------|--------|----------|
| SIMD Polygon Fill | 8x for opaque fill | `ct_render.c:fill_span()` |
| PNG Compression Level 2 | 3x faster encoding | `ct_png.c` |
| Mercator Lookup Table | Eliminates trig calls | `ct_tile.c` |
| Render Context Pool | No 262KB alloc/free per tile | `ct_png.c` |
| Batch Coordinate Transform | Reduced per-point overhead | `ct_tile.c` |
| PBF Hash Preallocation | 30-40% faster parsing | `ct_pbf.c` |
| Fast Alpha Blending | Integer approx for transparency | `ct_render.c` |

---

## Chapter 2: Tile Cache

### 2.1 LRU Cache Design (✅ Complete)

```c
typedef struct CTTileCache {
    struct {
        uint64_t *keys;      /* Packed z/x/y */
        uint8_t *data;       /* Tile bytes */
        size_t *sizes;       /* Per-tile size */
        uint32_t *lru;       /* LRU timestamps */
        size_t capacity;
        size_t used_bytes;
    } levels[19];            /* z0-z18 */

    size_t max_memory;       /* Total cache budget (e.g., 512MB) */
    uint32_t clock;          /* LRU clock */
} CTTileCache;
```

### 2.2 Eviction Policy

- Per-level memory limits (higher zoom = more tiles, less memory each)
- LRU within each level
- Recommended: 512MB total, ~50% to z14-z16 (most requested)

**Impact**: Cache hit = <1ms (vs ~75ms generation).

---

## Chapter 3: Level of Detail (LOD)

### 3.1 OSM-Like Zoom Rules (✅ Complete)

| Feature | Min Zoom | Notes |
|---------|----------|-------|
| **Roads** |
| Motorway | 5 | Full geometry |
| Trunk | 6 | Simplified at z6-z8 |
| Primary | 8 | |
| Secondary | 10 | |
| Tertiary | 12 | |
| Residential | 14 | |
| Service/Path | 15 | |
| **Water** |
| Ocean | 0 | |
| Large lakes (>100km²) | 4 | |
| Medium lakes (>1km²) | 8 | |
| Rivers (width>20m) | 8 | |
| Small water | 12 | |
| **Buildings** |
| Large (>5000m²) | 13 | |
| Medium (>500m²) | 14 | |
| All buildings | 15 | |

### 3.2 Road Width by Zoom and Class (✅ Complete)

```c
static const CTRoadStyle ROAD_STYLES[] = {
    [CT_ROAD_MOTORWAY]   = { 2.0, 4.0, 8.0, 0xFF4A6B8A, 0xFFE892A2 },
    [CT_ROAD_TRUNK]      = { 1.5, 3.5, 7.0, 0xFF8A6B4A, 0xFFFBB29A },
    [CT_ROAD_PRIMARY]    = { 1.0, 3.0, 6.0, 0xFF8A7B4A, 0xFFFCD6A4 },
    [CT_ROAD_SECONDARY]  = { 0.8, 2.5, 5.0, 0xFF9A9A5A, 0xFFF7FABF },
    [CT_ROAD_TERTIARY]   = { 0.6, 2.0, 4.0, 0xFFAAAAAA, 0xFFFFFFFF },
    [CT_ROAD_RESIDENTIAL]= { 0.4, 1.5, 3.0, 0xFFAAAAAA, 0xFFFFFFFF },
};
```

### 3.3 Geometry Simplification (✅ Complete)

Zoom-adaptive simplification matching visual resolution:

```c
double ct_lod_simplify_tolerance(int zoom) {
    double meters_per_pixel = 156543.03 / (1 << zoom);
    return meters_per_pixel * 0.5;  /* Half-pixel tolerance */
}
```

---

## Chapter 4: Label Rendering

### 4.1 Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                         Label Pipeline                              │
├────────────────────────────────────────────────────────────────────┤
│  1. Label Collection     Parse names from OSM features             │
│         ↓                                                          │
│  2. Label Placement      Calculate positions for each label        │
│         ↓                                                          │
│  3. Collision Detection  Remove overlapping labels                 │
│         ↓                                                          │
│  4. Rendering            Draw labels to PNG or encode to MVT       │
└────────────────────────────────────────────────────────────────────┘
```

### 4.2 Font System (✅ Complete)

Uses MSDF font library from `shared/`:

```c
#include "sh_font.h"

const SHFont *font = sh_font_get_default();
float width = sh_font_text_width(font, "Hello", 16.0f);
float coverage = sh_font_msdf_coverage(font, glyph, x, y, font_size);
```

**Font assets**: `clayshards/fonts/ui-font.json` + `ui-font.png` (163 glyphs, 332x332 atlas)

### 4.3 Point Labels (✅ Complete)

Cities, towns, villages, POIs with multi-anchor placement:

```c
typedef struct {
    int x, y;                /* Anchor point (tile coords) */
    int text_x, text_y;      /* Text position */
    CTAnchor anchor;         /* N, NE, E, SE, S, SW, W, NW, CENTER */
    int width, height;       /* Bounding box */
} CTLabelPlacement;
```

**Implementation**:
- Grid-based collision (1 bit per 8x8 cell)
- 9 anchor positions tried in order
- Priority by place type and population
- Font size scaled by place type

### 4.4 Road Labels (✅ Complete)

**Goal**: Road names rendered along path geometry.

**Algorithm**:
1. Measure text width at target font size
2. Find candidate segments with:
   - Length >= text width + padding
   - Max angle between segments < 30°
   - Prefer horizontal orientation
3. Check collision for text bounding boxes along path
4. Flip text if path goes right-to-left

```c
typedef struct {
    CTTilePoint *path;       /* Path for text to follow */
    int path_len;
    float start_offset;      /* Where along path to start */
    int is_flipped;          /* Flip to read left-to-right */
} CTLineLabelPlacement;

void ct_render_text_path(CTRenderContext *ctx,
                         const char *text,
                         const CTTilePoint *path,
                         int path_len,
                         float start_offset,
                         int is_flipped,
                         const SHFont *font,
                         float font_size,
                         CTColor fill_color,
                         CTColor halo_color,
                         float halo_width);
```

### 4.5 Area Labels (✅ Complete)

**Goal**: Lake, park, and forest names centered in polygons.

**Algorithm**:
1. Calculate pole of inaccessibility (point furthest from edges)
2. Check if text fits within polygon
3. For elongated shapes, rotate text to match orientation

### 4.6 MVT Labels (❌ TODO)

Add `labels` layer to MVT output for client-side rendering:

```json
{
  "id": "place-city",
  "type": "symbol",
  "source-layer": "labels",
  "filter": ["==", "class", "city"],
  "layout": {
    "text-field": "{name}",
    "text-size": 14
  }
}
```

### 4.7 Label Priority

| Label Type | Priority | Min Zoom | Font Size |
|------------|----------|----------|-----------|
| Country | 100 | 3 | 16-24 |
| City (>1M pop) | 85 | 6 | 14-16 |
| City (>100k) | 80 | 8 | 12-14 |
| Town | 70 | 10 | 11-13 |
| Village | 50 | 13 | 10-11 |
| Motorway | 50 | 8 | 10 |
| Primary Road | 45 | 12 | 10 |
| Residential | 20 | 16 | 8 |

### 4.8 Metatile Label Caching (✅ Complete)

Labels placed independently per tile can appear/disappear at tile boundaries. Metatile caching solves this by placing labels across 2x2 tile groups using a shared collision grid, then caching the result. Sub-tiles extract their portion from the cached metatile.

```c
/* Compute labels across a 2x2 metatile group */
CTMetatileLabelResult *ct_metatile_compute_labels(
    const CTPBFContext *pbf, CTMetatileCoord mt, int tile_size);

/* Extract labels for a single sub-tile from the cached result */
void ct_metatile_extract_subtile(
    const CTMetatileLabelResult *result,
    int sx, int sy,
    CTLabelPlacer *placer,
    CTRoadLabelPlacement **out_roads, size_t *out_road_count);
```

**Key files**: `ct_metatile.h`, `ct_metatile.c`

**Design**:
- Thread-safe cache with `pthread_rwlock` and LRU eviction
- Default capacity: 4096 metatile entries
- Refcounted results for safe eviction while in use
- Covers point labels, area labels, and road labels

---

## Chapter 5: Clipping & Simplification

### 5.1 Tile Boundary Clipping (✅ Complete)

Large polygons spanning multiple tiles are clipped to tile bounds with buffer:

```c
CTBBox clip_bbox = {
    .min_x = -buffer, .min_y = -buffer,
    .max_x = extent + buffer, .max_y = extent + buffer
};

ct_clip_multipolygon(polygon, num_rings, ring_ends, clip_bbox, ...);
```

### 5.2 Douglas-Peucker Simplification (✅ Complete)

Zoom-adaptive tolerance removes unnecessary points:

```c
double tolerance = ct_simplify_tolerance(zoom);
ct_simplify_line_inplace(points, &num_points, tolerance);
```

**Impact**: 20-30% smaller MVT files, faster client-side rendering.

---

## Chapter 6: Testing & Benchmarks

### Performance Benchmarks

```bash
# Single tile
time curl -s "http://localhost:8081/tiles/14/9058/5729.png" > /dev/null

# Batch (10 tiles)
for i in {5725..5735}; do
    time curl -s "http://localhost:8081/tiles/14/9058/$i.png" > /dev/null
done
```

### Current Test Coverage

- 143 carta unit tests + 21 metatile tests = 164 total
- Clipping, simplification, batch transform, render options
- Label collision detection tests
- Metatile coordinate, cache, and extraction tests

### Visual Comparison

Generate tiles at z10, z12, z14, z16 and compare with OSM for:
- Feature visibility
- Road widths
- Label placement

---

## Chapter 7: Implementation Roadmap

### Completed

1. ✅ R-Tree spatial index
2. ✅ Memory allocation optimization
3. ✅ Polygon sorting fix
4. ✅ Tile cache
5. ✅ LOD rules
6. ✅ Point label placement
7. ✅ Collision detection
8. ✅ Text rendering with halo
9. ✅ Road labels (text along paths)
10. ✅ Area labels (lakes, parks)
11. ✅ Metatile label caching (cross-tile consistency)
12. ✅ ETag support for conditional tile caching (FNV-1a hash, 304 Not Modified)

### Next Up

1. Road styling to match OSM Carto (see Chapter 9)
2. @2x retina tile support (see Chapter 10)

### Future

1. MVT label layer
2. Zoom-adaptive anti-aliasing
3. SIMD coordinate transformation
4. Tile pre-generation
5. International text (CJK, RTL)
6. Continental-scale sharding (see Chapter 8)
7. POI layer (amenity icons)
8. Data-driven styling (JSON style config)
9. PNG optimization (adaptive filters)
10. Config hot-reload

---

## Chapter 8: Continental-Scale Serving (❌ TODO)

For Europe/US-scale deployments, a single Carta instance cannot hold all data in memory.

### 8.1 Scale Requirements

| Scale | PBF Size | RAM Required | Single Instance? |
|-------|----------|--------------|------------------|
| Country (Hungary) | 300MB | ~4GB | ✅ Yes |
| Large Country (Germany) | 3.5GB | ~50GB | ⚠️ Expensive |
| Continental (Europe) | 25GB | ~300GB | ❌ Impractical |
| Planet | 70GB | ~1TB | ❌ No |

### 8.2 Buffer Zone Sharding

Each region extract includes a 50-100km buffer zone around its borders:

```
┌─────────────────────────────────────────────────┐
│                    France                        │
│    ┌──────────────────────────────────────┐     │
│    │         Buffer Zone (~50km)          │     │
├────┼──────────────────────────────────────┼─────┤
│    │         Buffer Zone (~50km)          │     │
│    └──────────────────────────────────────┘     │
│                    Germany                       │
└─────────────────────────────────────────────────┘
```

**Key insight**: With 50km buffers, most border tiles are fully contained in at least one shard's buffer.

### 8.3 Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     Continental Tile Service                     │
├─────────────────────────────────────────────────────────────────┤
│  Layer 1: CDN (Cloudflare/Fastly)                               │
│  ├── Caches rendered tiles                                       │
│  └── 90%+ hit rate for popular tiles                            │
│                                                                  │
│  Layer 2: Tile Router                                           │
│  ├── Routes by tile bbox → shard                                │
│  ├── Handles border tiles (multi-shard merge)                   │
│  └── Redis cache for recent tiles                               │
│                                                                  │
│  Layer 3: Regional Carta Instances                              │
│  ├── carta-dach (DE, AT, CH + 50km buffer)                      │
│  ├── carta-france (FR + 50km buffer)                            │
│  ├── carta-nordics (NO, SE, FI, DK + 50km buffer)               │
│  └── ... (8-10 regional shards for Europe)                      │
│                                                                  │
│  Layer 4: Overview Server                                        │
│  └── carta-overview (simplified planet, z0-10 only)             │
└─────────────────────────────────────────────────────────────────┘
```

### 8.4 Tile Classification

```c
typedef enum {
    TILE_SINGLE_SHARD,      // Fully within one shard (95%+ of requests)
    TILE_IN_BUFFER,         // In buffer zone, one shard can handle
    TILE_MULTI_SHARD        // Spans buffer boundary, needs merge (rare)
} TileShardType;
```

### 8.5 Hierarchical Zoom Levels

| Zoom | Data Source | Why |
|------|-------------|-----|
| 0-6 | Simplified planet extract | Coastlines, country borders, major cities |
| 7-10 | Continental extract (simplified) | Major roads, large water bodies |
| 11-18 | Regional shards with buffers | Full detail |

### 8.6 Shard Configuration

```yaml
# shards.yaml
shards:
  - id: "dach"
    name: "Germany, Austria, Switzerland"
    core_bbox: [5.87, 45.82, 17.16, 55.06]
    buffer_km: 50
    pbf_path: "/data/dach-buffered.osm.pbf"
    endpoint: "http://carta-dach:8081"

  - id: "france"
    name: "France"
    core_bbox: [-5.14, 41.33, 9.56, 51.09]
    buffer_km: 50
    pbf_path: "/data/france-buffered.osm.pbf"
    endpoint: "http://carta-france:8081"

overview:
  max_zoom: 10
  pbf_path: "/data/planet-simplified.osm.pbf"
  endpoint: "http://carta-overview:8081"
```

### 8.7 Buffered Extract Generation

```bash
#!/bin/bash
# Generate buffered regional extracts with osmium-tool
EUROPE_PBF="europe-latest.osm.pbf"

# Germany with 50km buffer
osmium extract -b 5.37,46.82,15.54,55.51 \
    $EUROPE_PBF -o germany-buffered.osm.pbf

# France with 50km buffer
osmium extract -b -5.64,40.88,10.06,51.54 \
    $EUROPE_PBF -o france-buffered.osm.pbf
```

### 8.8 Memory Budget per Shard

| Region | PBF Size | RAM Needed | Instance Type |
|--------|----------|------------|---------------|
| Germany + buffer | ~4GB | ~50GB | r6i.4xlarge |
| France + buffer | ~4GB | ~50GB | r6i.4xlarge |
| UK + Ireland | ~1.5GB | ~20GB | r6i.2xlarge |
| Nordics | ~2GB | ~25GB | r6i.2xlarge |
| Overview (z0-10) | ~500MB | ~8GB | r6i.xlarge |

**Total for Europe: ~10-12 instances, ~280GB total RAM, ~$4,150/month (AWS)**

### 8.9 Implementation Phases

**Phase 1: Buffered Shards** (2-3 weeks)
- [ ] Script to generate buffered extracts with osmium
- [ ] Shard configuration file format
- [ ] Simple nginx/HAProxy routing by region

**Phase 2: Smart Router** (2-3 weeks)
- [ ] Tile Router service (new component)
- [ ] R-tree index of shard boundaries
- [ ] Redis caching layer

**Phase 3: Feature Merger** (1-2 weeks)
- [ ] OSM ID tracking for deduplication
- [ ] Multi-shard tile generation

**Phase 4: Hierarchical Zoom** (1-2 weeks)
- [ ] Simplified planet extract for z0-10
- [ ] Zoom-based routing

**Phase 5: Apex Pre-computation** (4-6 weeks)
- [ ] Offline tile generation pipeline
- [ ] MBTiles or S3 storage backend
- [ ] Incremental updates (OSM diffs)

### 8.10 Pre-computed Tile Pyramid

| Zoom | Tiles | Storage (est.) |
|------|-------|----------------|
| 0-10 | ~1.4M | ~50GB |
| 0-12 | ~22M | ~750GB |
| 0-14 | ~350M | ~12TB |

**Strategy**: Pre-compute z0-12, on-demand for z13+ with aggressive caching.

---

## File Structure

```
carta/
├── include/
│   ├── ct_rtree.h         # R-Tree spatial index
│   ├── ct_cache.h         # Tile cache
│   ├── ct_label.h         # Label placement API
│   ├── ct_collision.h     # Collision detection
│   └── ct_metatile.h      # Metatile label cache API
├── src/
│   ├── ct_rtree.c         # R-Tree implementation
│   ├── ct_cache.c         # LRU cache
│   ├── ct_label.c         # Label placement
│   ├── ct_collision.c     # Collision grid
│   ├── ct_metatile.c      # Metatile label cache
│   ├── ct_clip.c          # Geometry clipping
│   ├── ct_simplify.c      # Douglas-Peucker
│   └── ct_render.c        # PNG rendering
```

---

## Chapter 9: Road Styling — OSM Carto Match (❌ TODO)

Current road rendering uses approximate colors and 3-point interpolated widths that don't match OSM Carto. This chapter covers what's needed to match OSM's visual quality.

### 9.1 Problem

The current road style has several issues:
- **Colors are approximate** — fill and casing colors don't match OSM Carto hex values
- **Widths use 3-point interpolation** — only z10/z14/z18 are specified, intermediate zooms interpolated. OSM Carto defines exact widths at every zoom level.
- **No DPR scaling** — style values are baked for 512px tiles. Should store 1× reference values (256px) and scale by `tile_size / 256` at render time to support any tile size.
- **Single casing width** — `ct_style_road_casing()` returns one value for all road types. OSM Carto has three casing categories: major, secondary, standard.
- **Per-road casing+fill** — roads draw casing+fill per segment, causing ugly junction artifacts where one road's casing overwrites another's fill. OSM Carto draws ALL casings first, then ALL fills.
- **No low-zoom colors** — at z5-z11 where roads have no casing, OSM uses more saturated colors.

### 9.2 Required Changes

#### A. Per-zoom width table (replaces 3-point interpolation)

Replace `CTRoadWidth { float z10, z14, z18 }` with `CTRoadWidth { float w[19] }` for per-zoom lookup z0-z18.

OSM Carto reference values (`@*-width-z*` variables, these ARE the total visual width):

| Road | z6 | z7 | z8 | z9 | z10 | z11 | z12 | z13 | z14 | z15 | z16 | z17 | z18 |
|------|----|----|----|----|-----|-----|-----|-----|-----|-----|-----|-----|-----|
| Motorway | 0.4 | 0.8 | 1.0 | 1.4 | 1.9 | 2.0 | 3.5 | 6.0 | 6.0 | 10.0 | 10.0 | 18.0 | 21.0 |
| Trunk | 0.4 | 0.6 | 1.0 | 1.4 | 1.9 | 1.9 | 3.5 | 6.0 | 6.0 | 10.0 | 10.0 | 18.0 | 21.0 |
| Primary | | | 1.0 | 1.4 | 1.8 | 1.8 | 3.5 | 5.0 | 5.0 | 10.0 | 10.0 | 18.0 | 21.0 |
| Secondary | | | | 1.0 | 1.1 | 1.1 | 3.5 | 5.0 | 5.0 | 9.0 | 10.0 | 18.0 | 21.0 |
| Tertiary | | | | | 0.7 | 0.7 | 2.5 | 4.0 | 5.0 | 9.0 | 10.0 | 18.0 | 21.0 |
| Residential | | | | | | | 0.5 | 2.5 | 3.0 | 5.0 | 6.0 | 12.0 | 13.0 |
| Service | | | | | | | | | 2.0 | 2.0 | 3.5 | 7.0 | 8.5 |

**Critical**: The `@*-width-z*` variable IS the total visual width. Casing is drawn INSIDE (as the outer line at total width, fill drawn on top at `total - 2*casing`). Do NOT add casing on top of these values.

#### B. Exact OSM Carto colors

Fill colors (from `road-colors-generated.mss`):
```
Motorway:    #e892a2   RGB(232, 146, 162)
Trunk:       #f9b29c   RGB(249, 178, 156)
Primary:     #fcd6a4   RGB(252, 214, 164)
Secondary:   #f7fabf   RGB(247, 250, 191)
Tertiary:    #ffffff
Residential: #ffffff
Service:     #ffffff
```

Casing colors:
```
Motorway:    #dc2a67   RGB(220, 42, 103)
Trunk:       #c84e2f   RGB(200, 78, 47)
Primary:     #a06b00   RGB(160, 107, 0)
Secondary:   #707d05   RGB(112, 125, 5)
Tertiary:    #8f8f8f   RGB(143, 143, 143)
Residential: #bbbbbb   RGB(187, 187, 187)
Service:     #bbbbbb   RGB(187, 187, 187)
```

Low-zoom colors (z5-z11, more saturated, no casing):
```
Motorway:    #e66e89   RGB(230, 110, 137)
Trunk:       #f6967a   RGB(246, 150, 122)
Primary:     #f4c37d   RGB(244, 195, 125)
Secondary:   #e7ed9d   RGB(231, 237, 157)
```

#### C. Per-road-type casing

Three casing categories matching OSM Carto:

| Category | Road Types | z11 | z12 | z13 | z14 | z15 | z17 |
|----------|-----------|-----|-----|-----|-----|-----|-----|
| Major (`@major-casing-width`) | motorway, trunk, primary | 0.3 | 0.5 | 0.5 | 0.6 | 0.7 | 1.0 |
| Secondary (`@secondary-casing-width`) | secondary | — | 0.3 | 0.35 | 0.35 | 0.7 | 1.0 |
| Standard (`@casing-width`) | tertiary, residential, service | — | 0.3 | 0.5 | 0.55 | 0.6 | 0.8 |

#### D. DPR scaling

Store all style values at 256px reference. At render time, scale all line widths by `dpr = tile_size / 256.0f`. This applies to roads, waterways, railways, buildings, boundaries, bridges.

#### E. Two-pass road rendering

Draw ALL casings (outer lines) first, then ALL fills on top. Prevents junction artifacts.

```
Current (per-road):    road A casing → road A fill → road B casing (overwrites A fill!) → road B fill
Correct (two-pass):    road A casing → road B casing → road A fill → road B fill
```

#### F. Low-zoom color logic

At z ≤ 11 when no casing is rendered, use the saturated low-zoom colors instead of the normal fill colors.

### 9.3 Lessons from Failed Attempts

Multiple attempts to match OSM Carto road widths were reverted. Key mistakes to avoid:

1. **Don't add casing outside the width variable** — the `@motorway-width-z14 = 6.0` IS the total. `fill = 6.0 - 2*0.6 = 4.8`. Do NOT compute `total = 6.0 + 2*0.6 = 7.2`.
2. **0.5f minimum width breaks zoom visibility** — roads with 0 width at a given zoom should not render. The min-visibility floor causes faint ghost lines at zooms where roads shouldn't appear.
3. **Test at 256px tile size** — use `carta-compare -s 256` to match OSM reference tiles pixel-for-pixel. 512px tiles with DPR=2 should look identical in proportion but comparing raw pixels is confusing.
4. **Anti-aliasing adds ~1px visual width** — Xiaolin Wu AA feathers edges, making a 3px line look ~5px. This is expected and matches how OSM Carto renders (Mapnik also uses AA).

### 9.4 Files to Modify

| File | Changes |
|------|---------|
| `ct_types.h` | Add `CTRoadWidth { float w[19] }`, add `road_low_zoom_colors[]` to CTStyle |
| `ct_style.c` | Per-zoom width table, exact colors, per-type casing function |
| `ct_render.c` | DPR scaling, two-pass road rendering, low-zoom color logic |
| `ct_render.h` | Declare `ct_style_road_casing_for_type()` |
| `test_carta.c` | Update width/casing test assertions |

---

## Chapter 10: @2x Retina Tiles (❌ TODO)

### 10.1 URL Convention

```
/tiles/{z}/{x}/{y}.png       → default tile_size (512px)
/tiles/{z}/{x}/{y}@2x.png    → tile_size * 2 (1024px)
```

### 10.2 What Already Scales

- Font sizes: `ct_label_base_font_size()` → `base * tile_size / 256.0f`
- Road label fonts: `fsize * tile_size / 256.0f`
- Label margins: derived from base_size

### 10.3 What Needs DPR Scaling (requires Chapter 9 first)

- Road widths, waterway widths, railway widths
- Bridge outline, boundary width
- Building outline width

### 10.4 Implementation Notes

- Parse `@2x` suffix in `parse_tile_uri()`
- Create render context at retina size (don't reuse thread-local 1x cache)
- Cache key: use bit 58 of packed key as retina flag (safe — z18 max x = 262143, 18 bits)
- Metatile cache: skip for @2x (key doesn't include tile_size), use per-tile labels
- MVT tiles don't need @2x (vector = resolution-independent)

---

## Chapter 11: ETag Support (✅ Complete)

### 11.1 Implementation

- FNV-1a 64-bit hash of tile bytes → 16-char hex ETag
- `ETag: "hexhash"` header on all tile responses (PNG, MVT, ASCII)
- `If-None-Match` header check → 304 Not Modified when client's ETag matches
- Applied to both direct render path and work queue path

### 11.2 Testing

```bash
# Get tile + capture ETag
curl -sI http://localhost:8081/tiles/14/8529/5974.png | grep ETag

# Conditional request → 304
curl -sI -H 'If-None-Match: "<etag>"' http://localhost:8081/tiles/14/8529/5974.png

# MVT also has ETag
curl -sI http://localhost:8081/tiles/14/8529/5974.mvt | grep ETag
```

### 11.3 Key Files

| File | Changes |
|------|---------|
| `api/src/main.c` | `fnv1a_64()`, `send_tile_cors()` ETag logic, `If-None-Match` check |

---

## Dependencies

| Library | Location | Purpose |
|---------|----------|---------|
| MSDF Font | `shared/sh_font.*` | Text measurement and rendering |
| miniz | `vendor/miniz/` | PNG compression |
| Font assets | `clayshards/fonts/` | ui-font.json, ui-font.png |

## Keel Migration (Mongoose Removal) — Phase 5 of 6

**Completed for Carta.** `carta/api` no longer links Mongoose.

Rationale and shared context: `docs/roadmaps/surge.md` (Phase 1).

### The concurrency model collapsed

Carta ran **N mongoose event-loop threads**, each with its own `mg_mgr`
listening on the same port via SO_REUSEPORT. That existed because every one of
those threads blocked in `render_work_item_wait()` for the duration of a
render — N loops was the only way to serve more than one tile at a time.

With `KlAsyncOp` the single Keel loop never blocks, so the multi-listener
design is gone: one event loop plus the render pool. `--threads` now sizes the
render pool rather than the number of listeners, and `WorkerThread`,
`worker_thread_fn` and the thread-local render-context key all went with it.

The three direct-render fallbacks (taken when the queue was disabled) also
went: `submit_render_work()` renders inline when there is no pool, so the
duplicated code paths collapsed into one. Net ~100 lines smaller.

### Transport replacements

| Mongoose | Replacement |
|----------|-------------|
| `mg_http_get_var()` | `sh_query_get_str()` |
| `mg_http_get_header()` | `kl_http_request_header()` / `sh_kl_origin()` |
| `mg_printf()` + `mg_send()` (tiles) | `kl_http_response_*` + `body_copy` |
| `mg_http_serve_dir()` | `serve_static_file()` (see below) |
| `mg_match()` | `strcmp()` / route table |

ETag and the conditional `304 Not Modified` are preserved exactly, now built
from `kl_http_request_header(req, "If-None-Match")`.

### Static files

`mg_http_serve_dir()` has no Keel equivalent, so `serve_static_file()` reads
the file and responds. **It rejects any path containing `..`** — mongoose did
that containment internally, and with Keel it is our job. Keel's own
`examples/static_files.c` registers `"/*"` as a *route*, which cannot match
(route patterns have no wildcard), so it was not usable as a model; static
serving lives in `mw_fallback` alongside the `/tiles/` prefix and the 404.

### Verification

Carta needs an OSM PBF, so CI was build + `--help` only. `carta/api/test_api.sh`
now starts the server against Monaco and gates on: health, stats, TileJSON,
MVT/PNG/ASCII tiles (including a real PNG signature check), ETag + 304,
error handling (bad format, zoom out of range, malformed path, 404), CORS on
both preflight and tile responses, and async dispatch under concurrent renders.

Not verified locally: Carta needs `mmap`/`sys/mman.h`, which MinGW lacks, so
unlike Velo this port could not be smoke-tested on Windows first. CI is its
first run.

### Remaining

Locus (63 `mg_` call sites) is the last module, then
`shared/src/sh_httpserver.c` (41) and `vendor/mongoose/` can be deleted.
