# Carta Tile Server Improvement Plan

## Executive Summary

This plan addresses three major areas:
1. **Performance**: R-Tree spatial index, tile caching, geometry optimization
2. **LOD Quality**: OSM-like zoom level rules, smooth transitions
3. **Label Rendering**: Street names, city names, POI labels

**Current State**: ~30ms per PNG tile, no spatial index (O(n) per tile), no labels
**Target State**: <10ms per tile, O(log n) queries, labeled maps

---

## Phase 1: Performance Optimization

### 1.1 R-Tree Spatial Index (Critical)

**Problem**: `ct_pbf_get_tile_features()` does O(n) linear scan of all ways for every tile request. Hungary has ~1M ways, making this unacceptable.

**Solution**: Implement packed Hilbert R-Tree

**Files to modify**:
- `carta/include/ct_types.h` - Add R-Tree structures
- `carta/src/ct_rtree.c` - NEW: R-Tree implementation
- `carta/src/ct_pbf.c` - Build R-Tree after parsing, use for queries

**Data structures**:
```c
#define CT_RTREE_MAX_ENTRIES 16
#define CT_RTREE_MIN_ENTRIES 4

typedef struct CTRTreeNode {
    CTBBox bbox;                    /* Bounding box of this node */
    int is_leaf;
    int count;                      /* Number of children/entries */
    union {
        struct CTRTreeNode *children[CT_RTREE_MAX_ENTRIES];
        uint32_t way_indices[CT_RTREE_MAX_ENTRIES];  /* Leaf: indices into ways array */
    };
} CTRTreeNode;

typedef struct CTRTree {
    CTRTreeNode *root;
    size_t node_count;
    size_t height;
} CTRTree;
```

**Algorithm**: Hilbert curve packing (Sort-Tile-Recursive)
1. Compute Hilbert index for each way's centroid
2. Sort ways by Hilbert index
3. Build tree bottom-up in O(n) time
4. Query: O(log n + k) where k = results

**Expected improvement**: 100-1000x faster tile queries for large datasets

### 1.2 Tile Cache (High Impact)

**Problem**: Every tile request regenerates from scratch

**Solution**: LRU cache with zoom-level partitioning

**Files to modify**:
- `carta/include/ct_cache.h` - NEW: Cache API
- `carta/src/ct_cache.c` - NEW: LRU implementation
- `carta/api/src/main.c` - Integrate cache

**Design**:
```c
typedef struct CTTileCache {
    /* Separate caches per zoom level (different access patterns) */
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

**Eviction policy**:
- Per-level memory limits (higher zoom = more tiles, less memory each)
- LRU within each level
- Recommended: 512MB total, ~50% to z14-z16 (most requested)

**Expected improvement**: Cache hit = <1ms (vs 30ms generation)

### 1.3 Geometry Clipping & Simplification in MVT Path

**Problem**: Lines 445-446 in ct_mvt.c have TODO for clipping/simplification

**Solution**: Apply existing `ct_clip_*` and `ct_simplify_*` functions

**Files to modify**:
- `carta/src/ct_mvt.c` - Add clipping and simplification calls

**Implementation**:
```c
/* In ct_mvt_encode_feature(), before encoding points: */

/* 1. Clip to tile bounds with buffer */
CTBBox clip_bbox = {
    .min_x = -buffer, .min_y = -buffer,
    .max_x = extent + buffer, .max_y = extent + buffer
};

if (feature->type == CT_GEOM_POLYGON) {
    ct_clip_polygon(feature->points, feature->num_points, clip_bbox, &clipped, &clipped_count);
} else if (feature->type == CT_GEOM_LINESTRING) {
    ct_clip_linestring(feature->points, feature->num_points, clip_bbox, &clipped, &clipped_count);
}

/* 2. Simplify based on zoom */
double tolerance = ct_simplify_tolerance(zoom);
ct_simplify_line_inplace(clipped, &clipped_count, tolerance);
```

**Expected improvement**: 20-30% smaller MVT files, faster client-side rendering

### 1.4 Render Pass Optimization

**Problem**: 5 passes over all features (one per layer)

**Solution**: Pre-sort features by layer during PBF parsing

**Files to modify**:
- `carta/src/ct_pbf.c` - Add layer indices
- `carta/src/ct_render.c` - Use layer indices

**Data structure addition to CTPBFContext**:
```c
struct {
    uint32_t *indices;    /* Way indices for this layer */
    size_t count;
} layer_ways[CT_LAYER_COUNT];
```

**Expected improvement**: 5x fewer feature iterations during rendering

### 1.5 Stack Allocation for Small Buffers

**Problem**: `malloc()` for every feature's scaled coordinates

**Solution**: Stack buffer with heap fallback

```c
#define CT_STACK_POINTS 256

void render_feature(...) {
    CTTilePoint stack_buf[CT_STACK_POINTS];
    CTTilePoint *scaled = (num_points <= CT_STACK_POINTS)
        ? stack_buf
        : malloc(num_points * sizeof(CTTilePoint));
    // ...
    if (scaled != stack_buf) free(scaled);
}
```

**Expected improvement**: Minor (~5% for small features)

---

## Phase 2: LOD Quality Improvements

### 2.1 OSM-Like Zoom Level Rules

**Current problems**:
- LOD disabled by default ("rules need improvement")
- Abrupt appearance of features
- Missing mid-zoom transitions

**Target**: Match OpenStreetMap's progressive disclosure

**New LOD rules** (based on OSM Carto):

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
| Streams | 14 | |
| **Places** |
| Countries | 3 | Label only |
| States/Regions | 5 | |
| Cities (pop>1M) | 6 | |
| Cities (pop>100k) | 8 | |
| Towns (pop>10k) | 10 | |
| Villages | 13 | |
| Suburbs/Hamlets | 14 | |
| **Buildings** |
| Large (>5000m²) | 13 | |
| Medium (>500m²) | 14 | |
| All buildings | 15 | |
| **Land use** |
| Forests (>10km²) | 8 | |
| Parks (>1km²) | 10 | |
| All landuse | 14 | |

### 2.2 Geometry Simplification by Zoom

**Current**: Fixed tolerance or none

**Target**: Zoom-adaptive simplification matching visual resolution

```c
/* Tolerance in meters based on zoom level */
double ct_lod_simplify_tolerance(int zoom) {
    /* At z0, 1 pixel ≈ 156km. Tolerance = 0.5 pixels in meters */
    double meters_per_pixel = 156543.03 / (1 << zoom);
    return meters_per_pixel * 0.5;  /* Half-pixel tolerance */
}

/* Minimum feature size to render (in pixels) */
double ct_lod_min_feature_size(int zoom, CTGeomType type) {
    switch (type) {
        case CT_GEOM_POLYGON: return 4.0;   /* 2x2 pixel minimum */
        case CT_GEOM_LINESTRING: return 2.0; /* 2 pixel length */
        default: return 1.0;
    }
}
```

### 2.3 Road Width by Zoom and Class

**Current**: Single width for all roads

**Target**: Width hierarchy matching OSM

```c
typedef struct {
    float width_z10;    /* Base width at z10 */
    float width_z14;    /* Width at z14 */
    float width_z18;    /* Width at z18 */
    uint32_t casing;    /* Border color (darker) */
    uint32_t fill;      /* Fill color */
} CTRoadStyle;

static const CTRoadStyle ROAD_STYLES[] = {
    [CT_ROAD_MOTORWAY]   = { 2.0, 4.0, 8.0, 0xFF4A6B8A, 0xFFE892A2 },
    [CT_ROAD_TRUNK]      = { 1.5, 3.5, 7.0, 0xFF8A6B4A, 0xFFFBB29A },
    [CT_ROAD_PRIMARY]    = { 1.0, 3.0, 6.0, 0xFF8A7B4A, 0xFFFCD6A4 },
    [CT_ROAD_SECONDARY]  = { 0.8, 2.5, 5.0, 0xFF9A9A5A, 0xFFF7FABF },
    [CT_ROAD_TERTIARY]   = { 0.6, 2.0, 4.0, 0xFFAAAAAA, 0xFFFFFFFF },
    [CT_ROAD_RESIDENTIAL]= { 0.4, 1.5, 3.0, 0xFFAAAAAA, 0xFFFFFFFF },
    [CT_ROAD_SERVICE]    = { 0.3, 1.0, 2.0, 0xFFBBBBBB, 0xFFFFFFFF },
};

float ct_road_width(int road_type, int zoom) {
    const CTRoadStyle *s = &ROAD_STYLES[road_type];
    if (zoom <= 10) return s->width_z10;
    if (zoom >= 18) return s->width_z18;
    /* Linear interpolation */
    float t = (zoom - 10) / 8.0f;
    return s->width_z10 + t * (s->width_z18 - s->width_z10);
}
```

### 2.4 Smooth Feature Transitions

**Problem**: Features pop in abruptly at threshold zoom

**Solution**: Fade-in over 1 zoom level

```c
float ct_lod_opacity(int feature_min_zoom, float current_zoom) {
    if (current_zoom < feature_min_zoom - 1) return 0.0f;
    if (current_zoom >= feature_min_zoom) return 1.0f;
    /* Fade in over 1 zoom level */
    return current_zoom - (feature_min_zoom - 1);
}
```

This requires fractional zoom support in the renderer.

---

## Phase 3: Label Rendering

### 3.1 Font Rasterization

**Approach**: Use `stb_truetype.h` (single-header, no dependencies)

**Files to add**:
- `vendor/stb/stb_truetype.h` - Font loading/rasterization
- `carta/include/ct_font.h` - Font API
- `carta/src/ct_font.c` - Glyph cache and rendering

**Font system design**:
```c
typedef struct {
    stbtt_fontinfo info;
    uint8_t *ttf_data;

    /* Glyph cache (pre-rendered at common sizes) */
    struct {
        int size;           /* Font size in pixels */
        uint8_t *atlas;     /* Glyph atlas texture */
        int atlas_width, atlas_height;
        stbtt_bakedchar glyphs[256];  /* ASCII subset for now */
    } cache[8];             /* Cache 8 sizes: 10,12,14,16,18,20,24,32 */
} CTFont;

/* API */
CTFont *ct_font_load(const char *ttf_path);
void ct_font_free(CTFont *font);

/* Render text to RGBA buffer */
void ct_font_render_text(CTFont *font, int size, const char *text,
                         uint8_t *pixels, int width, int height,
                         int x, int y, uint32_t color);

/* Get text dimensions */
void ct_font_measure_text(CTFont *font, int size, const char *text,
                          int *width, int *height);
```

### 3.2 Label Placement Algorithm

**Requirements**:
- No label-label overlap
- No label-road overlap (for area labels)
- Prefer centered placement
- Priority: cities > towns > villages > streets

**Data structures**:
```c
typedef struct {
    CTBBox bbox;         /* Screen-space bounding box */
    int priority;        /* Higher = more important */
    uint32_t feature_id; /* Which feature this labels */
    char text[64];
    int placed;          /* Successfully placed? */
} CTLabel;

typedef struct {
    CTLabel *labels;
    size_t count, capacity;

    /* Spatial index for overlap detection */
    /* Simple grid: 16x16 cells covering tile */
    uint32_t *grid_cells[16][16];  /* Lists of label indices per cell */
} CTLabelContext;
```

**Placement algorithm** (greedy with priority):
```c
void ct_labels_place(CTLabelContext *ctx) {
    /* Sort by priority descending */
    qsort(ctx->labels, ctx->count, sizeof(CTLabel), compare_priority_desc);

    for (size_t i = 0; i < ctx->count; i++) {
        CTLabel *label = &ctx->labels[i];

        /* Try placement positions: center, then offsets */
        CTPoint positions[] = {
            label_center(label),
            offset(label, -10, 0),
            offset(label, 10, 0),
            offset(label, 0, -10),
            offset(label, 0, 10),
        };

        for (int p = 0; p < 5; p++) {
            CTBBox candidate = bbox_at_position(label, positions[p]);
            if (!overlaps_any_placed(ctx, candidate)) {
                label->bbox = candidate;
                label->placed = true;
                add_to_grid(ctx, i);
                break;
            }
        }
    }
}
```

### 3.3 Curved Road Labels

**Algorithm**: Place text along path with letter spacing

```c
typedef struct {
    float x, y;     /* Position */
    float angle;    /* Rotation in radians */
} CTGlyphPlacement;

/* Place text along a path (road centerline) */
int ct_place_text_on_path(const CTTilePoint *path, int path_len,
                          const char *text, int font_size,
                          CTGlyphPlacement *out_placements) {
    /* 1. Find path segment with enough length */
    float text_width = measure_text(text, font_size);
    int start_idx = find_segment_for_length(path, path_len, text_width);
    if (start_idx < 0) return 0;  /* Path too short */

    /* 2. Calculate cumulative distances */
    float *distances = compute_cumulative_distances(path + start_idx, ...);

    /* 3. Place each glyph */
    float cursor = 0;
    for (int i = 0; text[i]; i++) {
        float glyph_width = measure_glyph(text[i], font_size);
        float mid_dist = cursor + glyph_width / 2;

        /* Interpolate position and angle on path */
        int seg = find_segment_at_distance(distances, mid_dist);
        float t = (mid_dist - distances[seg]) / (distances[seg+1] - distances[seg]);

        out_placements[i].x = lerp(path[seg].x, path[seg+1].x, t);
        out_placements[i].y = lerp(path[seg].y, path[seg+1].y, t);
        out_placements[i].angle = atan2(path[seg+1].y - path[seg].y,
                                        path[seg+1].x - path[seg].x);

        cursor += glyph_width + letter_spacing;
    }
    return strlen(text);
}
```

### 3.4 Label Zoom Rules

| Label Type | Min Zoom | Font Size | Priority |
|------------|----------|-----------|----------|
| Country | 3 | 16-24 | 100 |
| State/Region | 5 | 14-18 | 90 |
| City (>1M pop) | 6 | 14-16 | 85 |
| City (>100k) | 8 | 12-14 | 80 |
| Town | 10 | 11-13 | 70 |
| Village | 13 | 10-11 | 60 |
| Motorway | 8 | 10 | 50 |
| Primary Road | 12 | 10 | 45 |
| Secondary Road | 14 | 9 | 40 |
| Residential | 16 | 8 | 30 |
| POI (major) | 15 | 9 | 35 |
| POI (minor) | 17 | 8 | 20 |

---

## Implementation Order

### Sprint 1: Core Performance (Estimated: 1 week)
1. R-Tree implementation and integration
2. Layer pre-sorting
3. MVT clipping/simplification

### Sprint 2: Caching & LOD (Estimated: 1 week)
1. Tile cache implementation
2. New LOD rules (OSM-like)
3. Road width styling
4. Stack allocation optimization

### Sprint 3: Basic Labels (Estimated: 1-2 weeks)
1. Integrate stb_truetype
2. Font loading and glyph cache
3. Point label placement (cities, towns)
4. Label collision detection

### Sprint 4: Advanced Labels (Estimated: 1 week)
1. Curved road label placement
2. Label priority and zoom rules
3. Label fade-in transitions
4. Performance optimization

---

## Testing Strategy

### Performance Benchmarks
```bash
# Baseline (before optimizations)
./scripts/benchmark.sh carta

# Per-optimization benchmarks
time ./carta/api/carta-server data/hungary.pbf &
for z in 10 12 14 16; do
    for i in {1..100}; do
        curl -s "http://localhost:8081/tiles/$z/1234/567.png" > /dev/null
    done
done
```

### Visual Comparison
- Generate tiles at z10, z12, z14, z16
- Compare with OSM tile server at same locations
- Check: feature visibility, road widths, label placement

### Unit Tests to Add
- R-Tree: insertion, query, edge cases
- Cache: hit/miss, eviction, memory limits
- Labels: collision detection, path placement
- LOD: visibility rules, opacity transitions

---

## Dependencies

### New Files
| File | Purpose | LOC Estimate |
|------|---------|--------------|
| `carta/src/ct_rtree.c` | R-Tree implementation | 400 |
| `carta/src/ct_cache.c` | Tile cache | 300 |
| `carta/src/ct_font.c` | Font rendering | 500 |
| `carta/src/ct_label.c` | Label placement | 600 |
| `vendor/stb/stb_truetype.h` | Font rasterization | (external) |

### External Resources
- Font file: Noto Sans or similar (Apache 2.0 license)
- stb_truetype.h: Public domain, single header

---

## Success Metrics

| Metric | Current | Target |
|--------|---------|--------|
| PNG tile latency (z14) | ~30ms | <10ms |
| Tile query complexity | O(n) | O(log n) |
| Cache hit rate | 0% | >80% |
| MVT file size | Baseline | -25% |
| Label coverage | 0% | >90% of named features |
| Visual similarity to OSM | Low | High |

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| R-Tree complexity | High | Use well-tested algorithm (STR packing) |
| Font licensing | Medium | Use open fonts (Noto, Liberation) |
| Label placement perf | Medium | Limit labels per tile, use grid acceleration |
| Memory usage | Medium | Configurable cache limits, streaming |
| Unicode support | Low | Start with ASCII/Latin-1, extend later |
