# Velo Performance Improvement TODO

## Overview

Current performance on Hungary (2.7M nodes, 5.5M edges):
- Baseline A* bidirectional: 130-285ms
- With ALT (16 landmarks): 36-86ms (3-4x speedup)
- With vehicle profiles: no overhead (bitmask filtering)
- Target: <10ms (approaching OSRM)

## Recommendations: Closing the Gap Without CH

To reach <10ms without implementing Contraction Hierarchies, the most promising approaches in order of effort/reward:

### Priority 1: Arc Flags (Best ROI)
**Expected: 3-5x speedup → ~10-25ms**

Arc flags are the clearest path to sub-20ms queries:
- Partition graph into 64-128 regions (grid-based is simplest)
- Precompute 64-128 bit flags per edge indicating reachable regions
- During search: `if (!(edge->arc_flags & target_region_bit)) continue;`
- Prunes 70-90% of edges with a single bitmask AND operation
- Combines naturally with ALT and vehicle profiles

**Implementation sketch:**
```c
// Preprocessing: for each edge, run Dijkstra from edge.target
// Mark all regions reachable. Store as edge->arc_flags bitmask.

// Query: precompute target_region = region_of(target)
// In edge loop: if (!(edge->arc_flags & (1ULL << target_region))) continue;
```

**Preprocessing cost:** ~30 minutes for Hungary (parallelizable)
**Memory cost:** +8 bytes/edge (64 regions) = ~44MB

### Priority 2: Reach Pruning (Combines Well)
**Expected: 2-3x additional speedup when combined with Arc Flags**

Reach pruning eliminates nodes that can't be on any shortest path:
- Precompute reach(v) = max distance from any shortest path endpoint through v
- During search: `if (reach[v] < min(g[v], h[v])) continue;`
- Most effective for interior nodes far from highways

**Can be approximated:** Instead of exact reach, use "local reach" computed
via bounded Dijkstra. Faster preprocessing, still effective.

### Priority 3: Goal-Directed Arc Flags (GDAF)
**Expected: Combined 10-20x speedup → ~5-10ms**

Combine Arc Flags with ALT heuristics:
- Use ALT to determine search direction
- Use Arc Flags to prune edges not leading to target region
- The two techniques are orthogonal and multiply

### Priority 4: Hub Labeling (Alternative to CH)
**Expected: Sub-millisecond queries**

If <10ms is still not enough, hub labeling is simpler than CH:
- Precompute label sets L(v) for each node
- Query: find minimum L(s) ∩ L(t) intersection
- No graph search at all - pure table lookup
- Higher preprocessing cost but conceptually simpler than CH

**Tradeoff:** ~4-8GB memory for Hungary, but microsecond queries

## Recommended Implementation Order

1. **Arc Flags** (2-3 days)
   - Grid-based partitioning (simple, no external deps)
   - 64-bit flags per edge
   - Test with current ALT
   - Expected result: **~15ms**

2. **Approximate Reach** (1-2 days)
   - Bounded Dijkstra from each node (radius ~50km)
   - Store reach values (~4 bytes/node)
   - Add pruning check in search loop
   - Expected result: **~8-12ms**

3. **Evaluate** - if <10ms achieved, stop here

4. **Hub Labeling** (3-5 days) - only if <10ms still needed
   - Higher memory but guaranteed fast queries

## Current Optimizations Status

| Optimization | Status | Speedup | Combined |
|--------------|--------|---------|----------|
| Hilbert reordering | ✅ | 1.5-2x | 1.5-2x |
| ALT (16 landmarks) | ✅ | 3-4x | 4.5-6x |
| 4-ary heap | ✅ | ~10% | 5-7x |
| Vehicle profiles | ✅ | 0% overhead | - |
| Arc Flags | ⏳ | 3-5x est. | **15-35x** |
| Reach Pruning | ⏳ | 2-3x est. | **30-100x** |

**Bottom line:** Arc Flags alone should get us to ~15ms. Combined with Reach
Pruning, <10ms is achievable without Contraction Hierarchies.

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
| 4-ary heap | ✅ Done | ~10% | Better cache locality |
| More landmarks (32) | ✅ Done | 8.5x long routes | VL_MAX_LANDMARKS=64 |
| Fast distance heuristic | ✅ Done | ~10-15% | Equirectangular approx |
| SIMD landmark heuristic | ✅ Done | ~10% | Requires transposed layout |
| Bounded suboptimality (ε) | ✅ Done | 2-10% | opts.epsilon, trades quality for speed |
| Sorted adjacency lists | ✅ Done | marginal | Sorts edges by target during Hilbert reorder |
| Lazy heuristic | ❌ Skipped | N/A | Bottleneck is memory, not compute |
| SIMD haversine | ❌ Skipped | N/A | Single calls can't batch |
| Bidirectional ALT | ❌ Skipped | N/A | Complex potential function, minimal gain over unidirectional |
| Aggressive contraction | ⏳ Pending | | |
| Arc flags | ⏳ Pending | | |
| Reach pruning | ⏳ Pending | | |
| Transit nodes | ⏳ Pending | | |
| Contraction Hierarchies | 🔒 Deferred | | |

## Best Results (Hilbert + 32 landmarks + SIMD)

| Route | Baseline | Optimized | Speedup |
|-------|----------|-----------|---------|
| Budapest → Szeged | 154ms | **34ms** | 4.5x |
| Sopron → Nyíregyháza | 345ms | **32ms** | 10.8x |
| Pécs → Debrecen | 200ms | **35ms** | 5.7x |

**Preprocessing**: Hilbert 648ms + Landmarks 3.6s = ~4s total

**Memory tradeoff** (32 landmarks on Hungary):
- Default (with transpose): 2.6 GB - faster queries via SIMD
- LOWMEM (no transpose): 1.3 GB - ~10% slower queries
- Build with `make LOWMEM=1` to use less memory

## Lessons Learned

### SIMD Only Helps with Contiguous Memory
- SIMD on strided landmark access: ❌ No improvement (memory-bound)
- SIMD on transposed layout: ✅ ~10% improvement
- Prefetching strided data: ❌ No improvement (evicted before use)

### Key Insight
The landmark heuristic was bottlenecked by memory access patterns, not computation.
Restructuring data layout (transpose) was necessary before SIMD could help.
