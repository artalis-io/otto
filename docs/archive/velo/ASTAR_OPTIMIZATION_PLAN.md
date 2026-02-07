# A* Performance Optimization Plan

## Goal
Improve A* query time from ~76ms to <20ms on Hungary (2.7M nodes) without expensive preprocessing like Contraction Hierarchies.

## Current Performance Baseline
- Hungary: 76ms for 183km route, exploring 181K nodes
- Bottleneck: Too many nodes explored, heap operations

---

## Phase 1: Graph Preprocessing (Fast, One-time)

### 1.1 Edge Contraction for Degree-2 Nodes
**Estimated improvement: 20-30%**

Many road network nodes have exactly 2 edges (straight road segments). These can be contracted into single edges during graph loading.

```
Before: A --[5km]--> B --[3km]--> C --[7km]--> D
After:  A --[15km]--> D (B, C stored for path reconstruction)
```

**Implementation:**
- During graph finalization, identify degree-2 nodes
- Contract chains into single edges with summed distance/duration
- Store intermediate nodes for geometry reconstruction
- Reduces node count by ~40-60% on typical road networks

### 1.2 Remove Dead Ends
**Estimated improvement: 5-10%**

Identify and mark nodes that lead to dead ends (no path to most of the graph). Skip them during search.

**Implementation:**
- Run BFS from a central node to identify reachable nodes
- Mark unreachable nodes with a flag
- Skip marked nodes during A* expansion

---

## Phase 2: Better Heuristic (ALT Algorithm)

### 2.1 Landmarks and Triangle Inequality
**Estimated improvement: 40-60%**

The ALT (A* with Landmarks and Triangle inequality) algorithm uses precomputed distances to landmark nodes for tighter lower bounds.

**Key insight:** For landmarks L, the heuristic becomes:
```
h(v, t) = max over all landmarks L of:
  |dist(v, L) - dist(t, L)|
```

This is admissible and often much tighter than haversine.

**Implementation:**
1. Select 8-16 landmark nodes (spread geographically)
2. Precompute shortest paths from each landmark to all nodes (8-16 Dijkstra runs)
3. Store distances in arrays (16 × 2.7M × 4 bytes = 173 MB for 16 landmarks)
4. During A*, compute max triangle inequality bound

**Preprocessing time:** ~16 × 200ms = 3.2 seconds (acceptable)

### 2.2 Landmark Selection Strategies
- **Farthest selection:** Pick landmarks far from each other
- **Planar selection:** Pick nodes near graph bounding box corners
- **Random + refinement:** Start random, keep best performers

---

## Phase 3: Data Structure Optimizations

### 3.1 Bucket Heap / Radix Heap
**Estimated improvement: 10-20%**

Road networks have bounded edge weights. Use bucket-based priority queue instead of binary heap.

**Implementation:**
- Divide distance range into buckets (e.g., 1000 buckets of 1km each)
- O(1) insert, O(bucket_count) extract-min (amortized O(1))
- Much faster than binary heap for large graphs

### 3.2 Cache-Friendly Memory Layout
**Estimated improvement: 5-15%**

Reorder nodes by spatial locality (Hilbert curve ordering) to improve cache hits.

**Implementation:**
- Compute Hilbert curve index for each node's coordinates
- Sort nodes by Hilbert index during graph construction
- Adjacent nodes in search are likely adjacent in memory

### 3.3 SIMD Distance Computation
**Estimated improvement: 5-10%**

Vectorize haversine computation for multiple nodes simultaneously.

**Implementation:**
- Process 4 edge relaxations at once with AVX
- Batch distance computations where possible

---

## Phase 4: Search Optimizations

### 4.1 Early Termination Heuristics
**Estimated improvement: 10-20%**

Stop search earlier when confident we have the optimal path.

**Implementation:**
- Track ratio of (best found path) / (heuristic lower bound)
- If ratio < 1.01, likely optimal already
- Use statistical bounds from previous queries

### 4.2 Partial Expansion
**Estimated improvement: 5-10%**

Don't fully expand nodes that are clearly not on the optimal path.

**Implementation:**
- Only expand first K edges of high-degree nodes initially
- Re-expand if needed

### 4.3 Reuse Across Queries
**Estimated improvement: Variable**

For multiple queries with same source or target, reuse partial search trees.

**Implementation:**
- Cache recent search results
- Warm-start from nearby previous queries

---

## Phase 5: Preprocessing Summary

### Fast Preprocessing (< 10 seconds)
| Optimization | Time | Space | Improvement |
|--------------|------|-------|-------------|
| Degree-2 contraction | 2s | 0 | 20-30% |
| Dead end removal | 1s | 0 | 5-10% |
| Landmarks (16) | 4s | 173 MB | 40-60% |
| Node reordering | 2s | 0 | 5-15% |
| **Total** | **~10s** | **173 MB** | **60-80%** |

### Expected Final Performance
- Current: 76ms
- After optimizations: **15-30ms**

---

## Implementation Priority

### High Impact, Low Effort
1. **Degree-2 contraction** - Straightforward graph transformation
2. **Landmarks (ALT)** - Well-documented algorithm, significant gains

### Medium Impact, Medium Effort
3. **Bucket heap** - Replace binary heap
4. **Cache-friendly layout** - Hilbert curve reordering

### Lower Priority
5. **SIMD optimization** - Requires careful vectorization
6. **Early termination** - Needs tuning
7. **Query reuse** - Application-specific

---

## Implementation Plan

### Step 1: Degree-2 Contraction
```c
// In vl_graph.c
VLStatus vl_graph_contract_degree2(VLGraph *graph);
```
- Identify chains of degree-2 nodes
- Create contracted edges
- Store original geometry for reconstruction

### Step 2: ALT Landmarks
```c
// New file: vl_landmarks.c
typedef struct {
    uint32_t num_landmarks;
    uint32_t *landmark_nodes;
    double *dist_to_landmark;    // [node][landmark]
    double *dist_from_landmark;  // [node][landmark]
} VLLandmarks;

VLLandmarks *vl_landmarks_create(const VLGraph *graph, int num_landmarks);
double vl_landmarks_heuristic(const VLLandmarks *lm, uint32_t from, uint32_t to);
```

### Step 3: Bucket Heap
```c
// In vl_heap.c or new vl_bucket_heap.c
typedef struct {
    uint32_t *buckets;
    uint32_t num_buckets;
    uint32_t bucket_width;  // in millimeters
    uint32_t min_bucket;    // current minimum non-empty bucket
} VLBucketHeap;
```

---

## Verification

After each optimization:
1. Run test suite to verify correctness
2. Run OSRM comparison to verify distances match
3. Benchmark on Hungary to measure improvement
4. Profile to identify next bottleneck

---

## References

1. Goldberg & Harrelson (2005) - "Computing the Shortest Path: A* Search Meets Graph Theory" (ALT algorithm)
2. Geisberger et al. (2008) - "Contraction Hierarchies" (for comparison)
3. Delling et al. (2009) - "Engineering Route Planning Algorithms"
4. Sanders & Schultes (2007) - "Highway Hierarchies Hasten Exact Shortest Path Queries"
