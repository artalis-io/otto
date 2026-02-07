# Velo vs OSRM Performance Comparison & Improvement Plan

## Executive Summary

| Metric | OSRM | Velo (Current) | Gap |
|--------|------|----------------|-----|
| Query Time | < 1ms | 20-500ms | 20-500x |
| Preprocessing | Hours | Seconds | OSRM slower |
| Memory (Graph) | 2-8 GB | 50-300 MB | Velo better |
| Algorithm | Contraction Hierarchies | A* / Dijkstra | Fundamental |

**Key Insight**: OSRM trades preprocessing time (hours) and memory (GB) for sub-millisecond queries. Velo uses no preprocessing but pays at query time.

---

## Benchmark Comparison

### OSRM Performance (from published benchmarks)

| Region | Nodes | Query Time | Preprocessing | Memory |
|--------|-------|------------|---------------|--------|
| Germany | ~20M | < 1ms | ~1 hour | ~8 GB |
| Europe | ~180M | 1-5ms | ~8 hours | ~50 GB |
| Planet | ~400M | 5-10ms | ~24 hours | ~100 GB |

*Sources: [OSRM Wiki](https://wiki.openstreetmap.org/wiki/Open_Source_Routing_Machine), [MalaGIS](https://geo.malagis.com/osrm-open-source-routing-machine.html)*

### Velo Performance (Synthetic Grid Benchmarks)

| Grid Size | Nodes | Dijkstra | A* | Bidir* |
|-----------|-------|----------|-----|--------|
| 100x100 | 10K | ~5ms | ~3ms | timeout |
| 300x300 | 90K | ~80ms | ~40ms | timeout |
| 500x500 | 250K | ~300ms | ~150ms | timeout |

*\*Bidirectional algorithms timeout due to O(V×E) reverse edge lookup bug*

### Why OSRM Is Faster

1. **Contraction Hierarchies (CH)**: Pre-computes shortcuts that skip intermediate nodes
2. **Hierarchical Search**: Only explores ~1000 nodes for continental routes
3. **Memory Layout**: Cache-optimized data structures
4. **No Heuristic Computation**: CH doesn't need haversine at query time

---

## Current Velo Bottlenecks

### Critical Issues

#### 1. **No Reverse Graph Index** (Severity: CRITICAL)
```c
// Current: O(V × E) per backward expansion!
for (uint32_t i = 0; i < graph->num_nodes; i++) {
    for (uint32_t e = 0; e < node->edge_count; e++) {
        if (edge->target != u) continue;  // Scanning ALL edges
```
**Impact**: Makes bidirectional algorithms unusable. Should be O(degree).

#### 2. **Full Array Initialization** (Severity: HIGH)
```c
for (size_t i = 0; i < n; i++) {
    g_score[i] = VL_INF;      // Touches ALL memory
    parent[i] = VL_INVALID_NODE;
}
```
**Impact**: O(V) initialization even for short routes. For 10M nodes = 80MB touched.

#### 3. **Haversine Per Edge Relaxation** (Severity: MEDIUM)
```c
double f = tentative_g + heuristic(graph, v, target, opts->weight);
// heuristic() calls vl_haversine() with sin/cos/sqrt
```
**Impact**: Expensive trig operations on every edge relaxation.

#### 4. **No Spatial Index for Nearest Node** (Severity: MEDIUM)
```c
for (uint32_t i = 0; i < graph->num_nodes; i++) {
    double dist = vl_haversine(coord, node_coord);  // O(V) linear scan
```
**Impact**: Nearest node lookup is O(V) instead of O(log V).

#### 5. **Memory Allocation Per Query** (Severity: LOW)
```c
double *dist = malloc(n * sizeof(double));
VLHeap *heap = vl_heap_create(n);
```
**Impact**: Allocation overhead, cache pollution, no reuse.

---

## Improvement Plan

### Phase 1: Fix Critical Bugs (1-2 days)

#### 1.1 Add Reverse Graph Index
```c
typedef struct {
    uint32_t num_nodes;
    VLNode *nodes;
    uint32_t num_edges;
    VLEdge *edges;

    // NEW: Reverse graph for bidirectional search
    uint32_t *reverse_edge_start;  // Offset into reverse_edges
    uint32_t *reverse_edge_count;
    uint32_t *reverse_edges;       // Source nodes for each edge
} VLGraph;
```

**Build reverse index during graph finalization:**
```c
// Count incoming edges per node
for (uint32_t e = 0; e < num_edges; e++) {
    reverse_edge_count[edges[e].target]++;
}
// Compute offsets and fill reverse_edges
```

**Expected Speedup**: 100-1000x for bidirectional algorithms

### Phase 2: Algorithmic Improvements (3-5 days)

#### 2.1 Lazy Initialization with Timestamps
```c
typedef struct {
    double dist;
    uint32_t parent;
    uint32_t timestamp;  // Which query this data is from
} VLNodeState;

static uint32_t current_timestamp = 0;

// At query start: just increment timestamp
current_timestamp++;

// When accessing node:
if (state[node].timestamp != current_timestamp) {
    state[node].dist = VL_INF;
    state[node].parent = VL_INVALID_NODE;
    state[node].timestamp = current_timestamp;
}
```
**Expected Speedup**: 2-5x for short/medium routes

#### 2.2 Precomputed Heuristic Table (ALT Algorithm)
```c
// Precompute distances from 16 landmark nodes
double landmarks[16][MAX_NODES];  // ~500MB for 2M nodes

// Triangle inequality gives tighter bound than haversine
double heuristic_alt(uint32_t u, uint32_t target) {
    double max_bound = 0;
    for (int i = 0; i < 16; i++) {
        double bound = fabs(landmarks[i][target] - landmarks[i][u]);
        if (bound > max_bound) max_bound = bound;
    }
    return max_bound;
}
```
**Expected Speedup**: 2-3x for A* (tighter bounds = less exploration)

#### 2.3 Arc Flags / Reach Pruning
Pre-partition graph into regions, store flags indicating which regions each edge can reach.
```c
typedef struct {
    uint32_t target;
    uint32_t distance;
    uint16_t duration;
    uint16_t flags;
    uint32_t region_flags;  // Bit mask: which regions reachable
} VLEdge;

// During search, skip edges that can't reach target region
if (!(edge->region_flags & target_region_mask)) continue;
```
**Expected Speedup**: 3-10x (prunes many edges)

### Phase 3: Memory & Cache Optimization (2-3 days)

#### 3.1 Query Context Pool
```c
typedef struct {
    double *dist_fwd;
    double *dist_bwd;
    uint32_t *parent_fwd;
    uint32_t *parent_bwd;
    VLHeap *heap_fwd;
    VLHeap *heap_bwd;
    uint32_t timestamp;
} VLQueryContext;

// Create once, reuse for all queries
VLQueryContext *vl_query_context_create(const VLGraph *graph);
```
**Expected Speedup**: 1.2-1.5x (eliminates malloc overhead)

#### 3.2 Cache-Friendly Node Layout
```c
// Current: 24 bytes per node
typedef struct {
    VLCoordFixed coord;    // 8 bytes
    uint32_t edge_start;   // 4 bytes
    uint32_t edge_count;   // 4 bytes
    int64_t osm_id;        // 8 bytes  <- rarely needed during routing
} VLNode;

// Optimized: Split hot/cold data
typedef struct {
    uint32_t edge_start;   // 4 bytes - HOT
    uint32_t edge_count;   // 4 bytes - HOT
} VLNodeHot;  // 8 bytes, fits in cache line with neighbors

typedef struct {
    VLCoordFixed coord;
    int64_t osm_id;
} VLNodeCold;  // Accessed only for geometry/output
```
**Expected Speedup**: 1.3-1.5x (better cache utilization)

#### 3.3 SIMD Haversine (for batch operations)
```c
#include <immintrin.h>

void vl_haversine_batch_avx(const VLCoord *a, const VLCoord *b,
                            double *results, int count) {
    // Process 4 distances in parallel with AVX
    for (int i = 0; i < count; i += 4) {
        __m256d lat1 = _mm256_loadu_pd(&a[i].lat);
        // ... vectorized haversine
    }
}
```
**Expected Speedup**: 2-4x for nearest-node queries

### Phase 4: Advanced Algorithms (1-2 weeks)

#### 4.1 Contraction Hierarchies (CH)
Full CH implementation for OSRM-competitive performance:

```c
// Preprocessing (one-time, takes minutes-hours)
VLCHGraph *vl_ch_preprocess(const VLGraph *graph);

// Query (sub-millisecond)
VLStatus vl_ch_route(const VLCHGraph *ch, uint32_t source, uint32_t target,
                     VLRoute *route);

typedef struct {
    VLGraph *graph;
    uint32_t *node_order;      // Contraction order
    uint32_t *shortcuts;       // Shortcut edges
    uint32_t num_shortcuts;
} VLCHGraph;
```

**CH Query Algorithm:**
1. Forward search: only go "upward" in hierarchy
2. Backward search: only go "upward" in hierarchy
3. Meet at highest-rank node on path
4. Unpack shortcuts for full path

**Expected Performance**: < 1ms queries (matching OSRM)

#### 4.2 Hub Labeling (HL)
Even faster than CH but uses more memory:
```c
// For each node, store distances to/from "hub" nodes
typedef struct {
    uint32_t *forward_hubs;
    uint32_t *backward_hubs;
    float *forward_dists;
    float *backward_dists;
} VLHubLabels;

// Query: O(|labels|) ~ O(100) operations
double vl_hl_distance(const VLHubLabels *hl, uint32_t s, uint32_t t) {
    double min_dist = INF;
    // Merge sorted hub lists
    while (i < s_hubs && j < t_hubs) {
        if (fwd_hub[i] == bwd_hub[j]) {
            min_dist = min(min_dist, fwd_dist[i] + bwd_dist[j]);
        }
        // advance smaller
    }
    return min_dist;
}
```
**Expected Performance**: < 0.1ms queries

### Phase 5: Spatial Indexing (3-5 days)

#### 5.1 R-Tree for Nearest Node
```c
typedef struct VLRTreeNode {
    VLCoord bbox_min, bbox_max;
    union {
        struct VLRTreeNode *children[4];
        uint32_t node_indices[4];
    };
    uint8_t is_leaf;
    uint8_t count;
} VLRTreeNode;

// O(log N) nearest neighbor query
uint32_t vl_rtree_nearest(const VLRTree *tree, VLCoord query);
```

#### 5.2 Grid Index (Simpler Alternative)
```c
// Divide bounding box into 1000x1000 cells
typedef struct {
    uint32_t *cell_nodes;      // Node indices per cell
    uint32_t *cell_offsets;    // Start offset for each cell
    double cell_size_lat;
    double cell_size_lon;
} VLGridIndex;

// O(1) cell lookup + O(k) local search
uint32_t vl_grid_nearest(const VLGridIndex *grid, VLCoord query);
```
**Expected Speedup**: 100-1000x for nearest node queries

---

## Implementation Priority

| Priority | Improvement | Effort | Speedup | Cumulative |
|----------|-------------|--------|---------|------------|
| P0 | Reverse graph index | 1 day | 100x bidir | Bidir works |
| P1 | Lazy initialization | 1 day | 3x | 3x |
| P2 | Query context pool | 0.5 day | 1.3x | 4x |
| P3 | Grid spatial index | 1 day | 100x nearest | 4x routing |
| P4 | ALT heuristic | 2 days | 2x | 8x |
| P5 | Cache-friendly layout | 1 day | 1.3x | 10x |
| P6 | Arc flags | 3 days | 5x | 50x |
| P7 | Contraction Hierarchies | 1 week | 50x | 500x |

**After P0-P3**: Velo achieves ~10-50ms queries (usable for real applications)
**After P7**: Velo achieves <1ms queries (competitive with OSRM)

---

## Memory vs Speed Tradeoffs

| Configuration | Query Time | Preprocessing | Memory |
|---------------|------------|---------------|--------|
| Velo (current) | 50-500ms | 10-30s | 50-100 MB |
| Velo + Phase 1-3 | 10-50ms | 10-30s | 60-120 MB |
| Velo + ALT | 5-20ms | 1-5 min | 150-500 MB |
| Velo + Arc Flags | 2-10ms | 5-15 min | 100-200 MB |
| Velo + CH | 0.5-2ms | 30-60 min | 200-500 MB |
| OSRM (CH) | 0.2-1ms | 1-4 hours | 2-8 GB |

---

## Recommended Path Forward

### Short Term (This Week)
1. **Fix reverse graph** - Makes bidirectional algorithms work
2. **Add lazy initialization** - 3x speedup with minimal code change
3. **Add grid spatial index** - Fast nearest-node lookup

### Medium Term (This Month)
4. **Implement ALT landmarks** - Better heuristic bounds
5. **Add query context pooling** - Eliminate allocation overhead
6. **Cache-optimize data layout** - Better memory access patterns

### Long Term (Next Quarter)
7. **Implement Contraction Hierarchies** - Sub-millisecond queries
8. **Add MLD support** - Live traffic updates
9. **SIMD optimizations** - Maximum throughput

---

## References

- [OSRM Wiki](https://wiki.openstreetmap.org/wiki/Open_Source_Routing_Machine)
- [Contraction Hierarchies Paper](https://algo2.iti.kit.edu/schultes/hwy/contract.pdf)
- [ALT Algorithm](https://www.microsoft.com/en-us/research/publication/computing-the-shortest-path-a-search-meets-graph-theory/)
- [Hub Labeling](https://arxiv.org/abs/1103.4509)
- [Customizable Contraction Hierarchies](https://arxiv.org/pdf/1402.0402)
