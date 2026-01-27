# Velo Performance Improvement TODO

## Overview

Current performance on Hungary (2.7M nodes, 5.5M edges):
- Baseline A* bidirectional: 150-350ms
- With ALT + Hilbert: 40-130ms
- Target: <10ms (approaching OSRM)

## Quick Wins (Low Effort)

### 1. 4-ary Heap
- [ ] Implement 4-ary heap in `vl_heap.c`
- [ ] Benchmark against binary heap

**Why**: Each node has 4 children instead of 2, reducing tree height from log2(n) to log4(n). Better cache locality since 4 children fit in a cache line.

**Expected gain**: 10-20%

### 2. More Landmarks
- [ ] Test with 32 landmarks instead of 16
- [ ] Test with 64 landmarks
- [ ] Find optimal landmark count

**Why**: More landmarks = tighter lower bounds = less exploration. Diminishing returns after ~32.

**Expected gain**: 10-30%

### 3. SIMD Haversine
- [ ] Implement SSE/AVX vectorized haversine
- [ ] Process 4 distance calculations at once
- [ ] Fallback for non-SIMD platforms

**Why**: Haversine involves sin/cos which are slow. SIMD can compute 4 distances in parallel.

**Expected gain**: 5-15%

### 4. Lazy Heuristic Evaluation
- [ ] Only compute ALT heuristic when node is popped from queue
- [ ] Cache heuristic values to avoid recomputation

**Why**: Many nodes are pushed but never popped. Computing heuristic on push wastes cycles.

**Expected gain**: 5-10%

### 5. Aggressive Node Contraction
- [ ] Extend degree-2 contraction to degree-3/4 nodes
- [ ] Contract nodes where shortcuts don't increase edge count much
- [ ] Measure graph size reduction

**Why**: Current contraction only handles simple chains. Could reduce graph by additional 20-30%.

**Expected gain**: 10-20%

## Medium Effort Optimizations

### 6. Arc Flags
- [ ] Implement graph partitioning (e.g., METIS-style or grid-based)
- [ ] Precompute arc flags for each partition
- [ ] Modify A* to check arc flags before relaxing edges

**Why**: If target is in region R, we only need edges that can reach R. Prunes 70-90% of edges.

**Expected gain**: 3-5x speedup

**Algorithm**:
1. Partition graph into k regions (e.g., k=64)
2. For each edge, compute which regions it can reach
3. Store as a bitmask per edge
4. During search, skip edges where target region bit is 0

### 7. Reach Pruning
- [ ] Precompute reach values for all nodes
- [ ] Modify A* to prune low-reach nodes
- [ ] Combine with ALT heuristic

**Why**: A node's "reach" is the maximum distance it can be from either endpoint on any shortest path through it. If a node's reach < min(dist_from_source, dist_to_target), it can be pruned.

**Expected gain**: 2-4x speedup

**Algorithm**:
1. For each node v, compute reach(v) = max over all shortest paths P containing v of min(dist(start(P), v), dist(v, end(P)))
2. During search, prune if reach(v) < min(g(v), h(v))

### 8. Transit Node Routing
- [ ] Identify access nodes (nodes near source/target)
- [ ] Precompute all-pairs shortest paths between transit nodes
- [ ] Use table lookup for long-distance queries

**Why**: For long routes, there's a small set of "transit nodes" (highway junctions) that all paths pass through. Precompute paths between them.

**Expected gain**: 10-50x for long routes

## High Effort (Deferred)

### 9. Contraction Hierarchies
- [ ] Implement node ordering heuristic
- [ ] Implement contraction with shortcut creation
- [ ] Implement bidirectional upward-only search
- [ ] Implement shortcut unpacking

**Why**: Industry standard. Would achieve sub-millisecond queries.

**Expected gain**: 50-150x

**Status**: Deferred - significant implementation effort

## Implementation Priority

1. **4-ary heap** - Simple, isolated change
2. **More landmarks** - Just parameter tuning
3. **Lazy heuristic** - Small code change
4. **SIMD haversine** - Moderate, platform-specific
5. **Aggressive contraction** - Moderate complexity
6. **Arc flags** - Good effort/reward ratio
7. **Reach pruning** - Complex preprocessing
8. **Transit nodes** - Complex, best for long routes
9. **Contraction Hierarchies** - Maximum effort, maximum reward

## Progress Tracking

| Optimization | Status | Speedup | Notes |
|--------------|--------|---------|-------|
| Degree-2 contraction | ✅ Done | ~19% | |
| ALT (16 landmarks) | ✅ Done | 2.5-3.8x | |
| Hilbert reordering | ✅ Done | 1.5-2.1x | |
| 4-ary heap | ⏳ Pending | | |
| More landmarks | ⏳ Pending | | |
| Lazy heuristic | ⏳ Pending | | |
| SIMD haversine | ⏳ Pending | | |
| Aggressive contraction | ⏳ Pending | | |
| Arc flags | ⏳ Pending | | |
| Reach pruning | ⏳ Pending | | |
| Transit nodes | ⏳ Pending | | |
| Contraction Hierarchies | 🔒 Deferred | | |
