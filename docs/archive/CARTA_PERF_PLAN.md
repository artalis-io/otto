# Carta Tile Rendering Performance Plan

## Status: 80% Complete (Feb 2026)

| Sprint | Status | Notes |
|--------|--------|-------|
| Sprint 1: Memory allocation | ✅ Complete | Reusable scale/edge buffers |
| Sprint 2: Polygon sorting | ✅ Complete | Insertion sort O(n) |
| Sprint 3: R-Tree trust | ✅ Complete | Removed redundant checks |
| Sprint 4: Context pooling | ✅ Complete | Render context reuse |
| Sprint 5: Zoom-adaptive AA | ❌ TODO | Low priority |

## Executive Summary

Analysis revealed tile rendering was **memory-bound, not CPU-bound**. Primary bottlenecks addressed:

1. ✅ **Per-feature malloc/free** - Fixed with reusable buffers
2. ✅ **O(n²) polygon scanline sorting** - Fixed with insertion sort
3. ✅ **Redundant coordinate checks** - Fixed by trusting R-Tree
4. ✅ **Per-tile render buffer allocation** - Fixed with context pooling

**Result: ~75ms average tile latency** (down from ~240ms)

---

## Current Hot Path Analysis

### PNG Rendering Pipeline

```
Request → ct_pbf_get_tile_features → ct_render_from_pbf → ct_render_tile → ct_png_encode
              │                            │                    │
              │ R-Tree query               │ Coord transform    │ Per-feature:
              │ + Fine intersection        │ (every point)      │   malloc scaled[]
              │   check (redundant)        │                    │   render polygon/line
              │                            │                    │   free scaled[]
              ▼                            ▼                    ▼
           50ms                          5ms                 150ms
```

### Memory Allocation Pattern (Per Tile)

| Allocation | Frequency | Size | Impact |
|------------|-----------|------|--------|
| Feature scaling buffer | 500× | 1KB avg | **PRIMARY BOTTLENECK** |
| Render pixel buffer | 1× | 1MB | High (no reuse) |
| Polygon edge arrays | 50× | 4KB avg | Medium |
| **Total per tile** | — | ~1.7MB | ~100-150ms overhead |

---

## Optimization Plan

### Sprint 1: Memory Allocation (Critical - 60% speedup)

**Goal:** Eliminate per-feature malloc/free in render loop

#### 1.1 Add Scaling Buffer to Render Context

**File:** `carta/include/ct_render.h`

```c
typedef struct {
    uint8_t *pixels;
    int width, height, stride;
    CTStyle style;

    /* NEW: Pre-allocated buffers for rendering */
    CTTilePoint *scale_buffer;    /* Reusable point buffer */
    size_t scale_buffer_capacity;
    CTEdge *edge_buffer;          /* Reusable edge buffer */
    size_t edge_buffer_capacity;
} CTRenderContext;
```

#### 1.2 Update Render Context Creation

**File:** `carta/src/ct_render.c`

```c
CTRenderContext *ct_render_create(int width, int height) {
    CTRenderContext *ctx = calloc(1, sizeof(CTRenderContext));
    ctx->pixels = calloc(width * height * 4, 1);
    ctx->width = width;
    ctx->height = height;
    ctx->stride = width * 4;

    /* Pre-allocate buffers for typical tile complexity */
    ctx->scale_buffer_capacity = 8192;  /* 8K points */
    ctx->scale_buffer = malloc(ctx->scale_buffer_capacity * sizeof(CTTilePoint));

    ctx->edge_buffer_capacity = 1024;   /* 1K edges */
    ctx->edge_buffer = malloc(ctx->edge_buffer_capacity * sizeof(CTEdge));

    ct_default_style(&ctx->style);
    return ctx;
}
```

#### 1.3 Eliminate Per-Feature Malloc in Render Loop

**File:** `carta/src/ct_render.c` (ct_render_tile function)

```c
// BEFORE (line ~480):
CTTilePoint *scaled = malloc(f->num_points * sizeof(CTTilePoint));
// ... render ...
free(scaled);

// AFTER:
static CTTilePoint *get_scale_buffer(CTRenderContext *ctx, size_t needed) {
    if (needed > ctx->scale_buffer_capacity) {
        ctx->scale_buffer_capacity = needed * 2;
        ctx->scale_buffer = realloc(ctx->scale_buffer,
                                     ctx->scale_buffer_capacity * sizeof(CTTilePoint));
    }
    return ctx->scale_buffer;
}

// In render loop:
CTTilePoint *scaled = get_scale_buffer(ctx, f->num_points);
// ... render (no free needed) ...
```

**Impact:** Eliminate 500+ malloc/free pairs → **100-150ms saved**

---

### Sprint 2: Polygon Rendering (High - 25% speedup)

**Goal:** Replace O(n²) bubble sort with O(n) insertion sort

#### 2.1 Fix Active Edge Table Sorting

**File:** `carta/src/ct_render.c` (ct_render_polygon function)

```c
// BEFORE (lines 295-307) - Bubble sort O(n²):
for (int i = 0; i < num_active - 1; i++) {
    for (int j = i + 1; j < num_active; j++) {
        if (active[j].x < active[i].x) {
            CTEdge t = active[i];
            active[i] = active[j];
            active[j] = t;
        }
    }
}

// AFTER - Insertion sort O(n) for nearly-sorted:
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

#### 2.2 Use Pre-allocated Edge Buffer

```c
// In ct_render_polygon:
CTEdge *edges = get_edge_buffer(ctx, num_points);  // Use context buffer
// ... build edge table ...
// No free needed
```

**Impact:** Polygon sort from O(n²) to O(n) → **80-120ms saved**

---

### Sprint 3: Feature Retrieval (Medium - 15% speedup)

**Goal:** Trust R-Tree results, remove redundant intersection checks

#### 3.1 Simplify ct_pbf_get_bbox_features

**File:** `carta/src/ct_pbf.c`

```c
// BEFORE (lines 877-895):
for (size_t i = 0; i < num_candidates; i++) {
    uint32_t way_idx = candidates[i];
    const CTOSMWay *way = &ctx->ways[way_idx];

    /* Fine-grained intersection check - REDUNDANT */
    int intersects = 0;
    for (int j = 0; j < way->num_coords; j++) {
        if (way->coords[j].lat >= bbox.min_lat && ...) {
            intersects = 1;
            break;
        }
    }
    if (!intersects) continue;  // Often false positive from R-Tree

    add_way_as_feature(way, ...);
}

// AFTER - Trust R-Tree bbox overlap:
for (size_t i = 0; i < num_candidates; i++) {
    uint32_t way_idx = candidates[i];
    if (way_idx >= ctx->num_ways) continue;

    const CTOSMWay *way = &ctx->ways[way_idx];

    /* R-Tree already filtered by bbox - just add feature */
    /* Optional: Quick bbox check only (not per-point) */
    add_way_as_feature(way, features, count, capacity);
}
```

#### 3.2 Add Way BBox Cache (Optional)

Store pre-computed bounding boxes for ways in binary index:

```c
typedef struct {
    // ... existing fields ...
    CTBBox bbox;  /* Pre-computed bounding box */
} CTOSMWay;
```

**Impact:** Skip 100K+ coordinate comparisons → **20-50ms saved**

---

### Sprint 4: Render Context Pooling (Medium - 10% speedup)

**Goal:** Reuse render contexts across requests

#### 4.1 Add Context Pool to Tile Server

**File:** `carta/api/src/main.c`

```c
#define RENDER_POOL_SIZE 4

typedef struct {
    CTRenderContext *contexts[RENDER_POOL_SIZE];
    int available[RENDER_POOL_SIZE];
    pthread_mutex_t lock;
} RenderPool;

static RenderPool s_render_pool;

CTRenderContext *acquire_render_context(void) {
    pthread_mutex_lock(&s_render_pool.lock);
    for (int i = 0; i < RENDER_POOL_SIZE; i++) {
        if (s_render_pool.available[i]) {
            s_render_pool.available[i] = 0;
            pthread_mutex_unlock(&s_render_pool.lock);
            ct_render_clear(s_render_pool.contexts[i], bg_color);
            return s_render_pool.contexts[i];
        }
    }
    pthread_mutex_unlock(&s_render_pool.lock);
    return ct_render_create(512, 512);  /* Fallback: create new */
}

void release_render_context(CTRenderContext *ctx) {
    pthread_mutex_lock(&s_render_pool.lock);
    for (int i = 0; i < RENDER_POOL_SIZE; i++) {
        if (s_render_pool.contexts[i] == ctx) {
            s_render_pool.available[i] = 1;
            pthread_mutex_unlock(&s_render_pool.lock);
            return;
        }
    }
    pthread_mutex_unlock(&s_render_pool.lock);
    ct_render_free(ctx);  /* Not from pool, free it */
}
```

**Impact:** Eliminate 1MB alloc per tile → **2-5ms saved** per tile

---

### Sprint 5: Anti-Aliasing Optimization (Low - 5% speedup)

**Goal:** Disable expensive AA at low zoom levels

#### 5.1 Add Zoom-Adaptive Line Drawing

**File:** `carta/src/ct_render.c`

```c
void ct_render_line(CTRenderContext *ctx, int x0, int y0, int x1, int y1,
                    CTColor color, float width, int zoom) {
    if (zoom < 12) {
        /* Low zoom: Use fast non-AA line (Bresenham) */
        draw_line_fast(ctx, x0, y0, x1, y1, color, (int)width);
    } else {
        /* High zoom: Use anti-aliased line (Xiaolin Wu) */
        draw_line_aa(ctx, x0, y0, x1, y1, color, width);
    }
}

/* Fast non-AA line using Bresenham */
static void draw_line_fast(CTRenderContext *ctx, int x0, int y0, int x1, int y1,
                           CTColor color, int width) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        /* Draw thick point */
        for (int wy = -width/2; wy <= width/2; wy++) {
            for (int wx = -width/2; wx <= width/2; wx++) {
                ct_render_set_pixel(ctx, x0 + wx, y0 + wy, color);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
```

**Impact:** Skip expensive blending at low zoom → **15-25ms saved**

---

## Implementation Order

| Sprint | Focus | Impact | Effort | Priority |
|--------|-------|--------|--------|----------|
| 1 | Memory allocation | 60% | Medium | **Critical** |
| 2 | Polygon sorting | 25% | Low | High |
| 3 | R-Tree trust | 15% | Low | High |
| 4 | Context pooling | 10% | Medium | Medium |
| 5 | Zoom-adaptive AA | 5% | Low | Low |

**Recommended order:** Sprint 1 → 2 → 3 → 4 → 5

---

## Verification

After each sprint:

```bash
# Benchmark single tile
time curl -s "http://localhost:8081/tiles/14/9058/5729.png" > /dev/null

# Benchmark batch (10 tiles)
for i in {5725..5735}; do
    time curl -s "http://localhost:8081/tiles/14/9058/$i.png" > /dev/null
done

# Memory profiling
valgrind --tool=massif ./carta/api/carta-tile-server -p 8081 data/index/hungary-carta.idx
```

**Target metrics:**
- Single tile: <100ms (currently ~240ms)
- Memory allocations per tile: <10 (currently ~500)
- Cache hit: skip rendering entirely

---

## Future Optimizations (Post-Sprint)

1. **SIMD coordinate transformation** - Use SSE/AVX for batch transforms
2. **GPU rasterization** - Offload polygon fill to compute shaders
3. **Tile pre-generation** - Background process for common zoom levels
4. **Geometry simplification cache** - Store simplified versions per zoom
5. **Parallel scanline fill** - Use OpenMP for polygon rows
