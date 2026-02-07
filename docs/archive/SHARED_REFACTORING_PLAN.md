# Shared Building Blocks Consolidation Plan

**Created:** 2026-02-05
**Status:** ✅ Complete (Phase 1 + Phase 2 done, Phase 3 optional/deferred)

## Summary

Analysis of carta/, velo/, and locus/ identified multiple duplicated data structures that should be consolidated into the shared/ library for code reuse, consistency, and reduced maintenance.

## Key Findings

### Currently Duplicated

| Structure | Carta | Velo | Locus | Recommendation |
|-----------|-------|------|-------|----------------|
| Hash Table | node_map (open addressing) | VLNodeMap (chaining) | LCNgramBucket | Create `sh_hashmap.h` |
| Spatial Grid | - | VLGridIndex | LCSpatialGrid | Create `sh_spatial_grid.h` |
| Web Mercator | ct_tile.c | - | - | Use existing `sh_geo.h` |
| Bbox ops | ct_rtree.c | - | - | Use existing `sh_bbox_*` |

### Velo-Only (Consider Sharing)

| Structure | Location | Notes |
|-----------|----------|-------|
| 4-ary Heap | vl_heap.c | Useful for any priority queue (work queues, schedulers) |

### Locus-Only (Consider Sharing)

| Structure | Location | Notes |
|-----------|----------|-------|
| Prefix Trie | lc_trie.c | Specialized to 37-char geocoding alphabet |
| N-gram Index | lc_ngram.c | Trigram fuzzy matching |
| Text Normalize | lc_normalize.c | UTF-8 case folding, diacritics |

## Implementation Plan

### Phase 1: High-Impact Consolidation

#### 1.1 Create `sh_hashmap.h`
**Files to create:**
- `shared/include/sh_hashmap.h`
- `shared/src/sh_hashmap.c`

**Features:**
- Generic key-value storage (void* or typed macros)
- Support both separate chaining and open addressing
- Configurable hash functions (FNV-1a default)
- Auto-resize at configurable load factor (default 0.75)
- Iterator support

**Then update:**
- velo/src/vl_graph.c: Replace VLNodeMap with sh_hashmap
- carta/src/ct_pbf.c: Replace node_map/way_map with sh_hashmap

#### 1.2 Create `sh_heap.h`
**Files to create:**
- `shared/include/sh_heap.h`
- `shared/src/sh_heap.c`

**Features:**
- 4-ary min-heap (better cache locality than binary)
- Position tracking for O(log n) decrease-key
- Generic entry type with customizable comparator
- Max capacity specified at creation

**Then update:**
- velo/src/vl_heap.c: Use sh_heap or keep as thin wrapper

#### 1.3 Create `sh_spatial_grid.h` — NOT INTEGRATED
**Files created:**
- `shared/include/sh_spatial_grid.h`
- `shared/src/sh_spatial_grid.c`

**Features:**
- Uniform grid with configurable cell size
- Point insertion and query
- Radius query support
- Nearest-neighbor with distance callback
- CSR format option for read-only grids (memory efficient)

**Decision:** NOT integrated into velo/locus due to domain-specific optimizations:
- Velo's VLGridIndex uses 1000x1000 fixed grid with OpenMP parallel construction
- Locus's LCSpatialGrid uses geometry-aware distance calculation
- Generic version would sacrifice these optimizations without significant code reduction

### Phase 2: Housekeeping (Remove Duplicates) ✅ COMPLETE

#### 2.1 Carta Web Mercator Consolidation ✅
**Files updated:**
- carta/src/ct_tile.c: `ct_latlon_to_mercator()` now delegates to `sh_latlon_to_mercator()`

#### 2.2 Carta Bbox Consolidation ✅
**Files updated:**
- carta/include/ct_types.h: `CTBBox` is now a typedef to `SHBBox`
- carta/src/ct_rtree.c: `bbox_union`, `bbox_intersects` are now macros for `sh_bbox_*` functions

### Phase 3: Optional Extraction (Lower Priority)

#### 3.1 Text Normalization (if needed elsewhere)
- Extract lc_normalize.c as sh_text_normalize.h
- Would need UTF-8 tables and normalization functions
- Only if another project needs this capability

#### 3.2 N-gram Index (if needed elsewhere)
- Extract as sh_ngram.h
- Trigram generation and Jaccard similarity
- Only if another project needs fuzzy text matching

#### 3.3 Prefix Trie (if needed elsewhere)
- Extract as sh_trie.h
- Parameterize alphabet (currently 37-char for geocoding)
- Only if another project needs autocomplete

## Not Recommended for Sharing

| Structure | Reason |
|-----------|--------|
| VLGraph (CSR) | Highly specialized to road routing with vehicle profiles |
| CTRTree | Hilbert-packed R-Tree is specialized to range queries |
| Contraction | Routing-specific optimization |

## Effort Estimates

| Task | Files | Lines | Risk |
|------|-------|-------|------|
| sh_hashmap.h | 2 new, 4 update | ~400 new, ~200 removed | Medium |
| sh_heap.h | 2 new, 1 update | ~200 new, ~50 removed | Low |
| sh_spatial_grid.h | 2 new, 4 update | ~500 new, ~400 removed | Medium |
| Mercator consolidation | 2 update | ~50 removed | Low |
| Bbox consolidation | 1 update | ~30 removed | Low |

## Dependencies

```
Phase 1 can be done in any order:
  1.1 sh_hashmap → update velo, carta
  1.2 sh_heap → update velo
  1.3 sh_spatial_grid → update velo, locus

Phase 2 depends on Phase 1 being complete for testing

Phase 3 is independent and optional
```

## Test Strategy

1. Create unit tests for each new shared component
2. Run existing module tests after each update to catch regressions
3. Benchmark before/after for performance-critical paths (hash lookups, spatial queries)

## Verification

```bash
# After each phase
make test              # All tests should pass
make bench             # Performance regression check

# Actual test counts after Phase 1:
# shared: 206 tests (includes hashmap, heap, spatial grid)
# velo: 51 tests (uses sh_hashmap, sh_heap)
# carta: 110 tests (uses sh_hashmap)
# locus: 52 tests (no changes - keeps domain-specific structures)
```

## Reference: Existing Shared Library

The shared library already provides:
- `sh_geo.h`: Coordinates, haversine, bearing, Web Mercator, tile operations
- `sh_arena.h`: Arena allocator
- `sh_pool.h`: Object pool allocator
- `sh_workqueue.h`: Bounded work queue
- `sh_ratelimit.h`: Token bucket rate limiting
- `sh_protobuf.h`: Protobuf encoding/decoding
- `sh_pbf.h`: PBF parsing utilities
- `sh_inflate.h`: Zlib decompression
