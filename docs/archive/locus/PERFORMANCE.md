# Locus Performance Analysis and Improvement Plan

## Benchmark Results (Hungary - 1.1M entities)

| Operation | Current | Target | Improvement |
|-----------|---------|--------|-------------|
| PBF Loading | 53.7s | <10s | 5x faster |
| Binary Load | 43.4s | <1s | 40x faster |
| Fuzzy Search | 34.8ms | <100µs | 350x faster |
| Miss Search | 4.1ms | <10µs | 400x faster |
| Memory Usage | 1.9GB | <500MB | 4x smaller |
| Exact Search | 2.0µs | <2µs | ✓ (OK) |
| Reverse Geocode | 24.8µs | <25µs | ✓ (OK) |

## Critical Bottlenecks

### 1. Fuzzy Search - O(n²) Hit Tracking (CRITICAL)

**Location:** `lc_ngram.c:224-249`

**Problem:** Linear scan to find/add entity hits:
```c
for (size_t k = 0; k < hits_count; k++) {
    if (hits[k].entity_id == eid) {  // O(n) per hit!
        found = (int)k;
        break;
    }
}
```

With 1.1M entities and common trigrams matching 100K+ entities, this becomes O(n²).

**Solutions:**

| Approach | Complexity | Implementation |
|----------|------------|----------------|
| Hash table for hits | O(1) lookup | Replace linear array with hash map |
| Bitmap + count array | O(1) lookup | uint8_t count[num_entities] |
| Inverted posting lists | O(k) merge | Pre-sorted entity lists with merge |
| Limit candidate set | O(n) bounded | Cap to top-k by posting list length |

**Recommended:** Bitmap approach - allocate `uint32_t hits[num_entities]` as scratch space, O(1) increment per hit.

### 2. Binary Index Rebuild (CRITICAL)

**Location:** `lc_serialize.c`, `lc_index.c:82-114`

**Problem:** Binary load saves only entity data, then rebuilds all indexes:
- Trie: O(n × m) where m = avg name length
- N-gram: O(n × m) + O(k log k) sort
- Spatial grid: O(n)

Total rebuild is ~80% of PBF parse time.

**Solutions:**

| Approach | Disk Size | Load Time | Complexity |
|----------|-----------|-----------|------------|
| Serialize trie | +20-50MB | <1s | Medium |
| Serialize n-gram | +30-100MB | <1s | Medium |
| mmap() indexes | 0 overhead | <100ms | High |
| Lazy rebuild | 0 overhead | Amortized | Low |

**Recommended (Phase 1):** Serialize trie and spatial grid. N-gram can be rebuilt (it's sorted, fast).

**Recommended (Phase 2):** mmap() the entire index file for instant loading.

### 3. Memory Usage (HIGH)

**Location:** `lc_types.c`, entity storage

**Problem:** Each entity has individually malloc'd strings:
- Name: 8-byte pointer + malloc overhead (16-32 bytes)
- Alt names: array of pointers
- Address fields: 7 pointers
- POI type: 1 pointer

With 1.1M entities × ~10 strings each = 11M allocations.

**Solutions:**

| Approach | Memory Saved | Complexity |
|----------|--------------|------------|
| String pool with offsets | 40-50% | Medium |
| Deduplicate common strings | 10-30% | Low |
| Compact entity struct | 20-30% | Medium |
| Memory-mapped strings | 50%+ | High |

**Recommended:** String pool with 32-bit offsets instead of 64-bit pointers.

### 4. PBF Parsing (MEDIUM)

**Location:** `lc_pbf.c`

**Problem:** Single-threaded parsing of 300MB file.

**Solutions:**

| Approach | Speedup | Complexity |
|----------|---------|------------|
| Parallel blob processing | 2-4x | Medium |
| mmap() instead of fread | 1.2-1.5x | Low |
| SIMD varint decoding | 1.5-2x | High |

**Recommended:** Use mmap() and process blobs in parallel (each PrimitiveBlock is independent).

---

## Implementation Plan

### Phase 1: Fix Fuzzy Search (1-2 days)

**Goal:** Reduce fuzzy search from 34.8ms to <100µs

1. **Replace linear hit array with bitmap:**
   ```c
   // Allocate once per search
   uint16_t *hit_counts = calloc(index->num_entities, sizeof(uint16_t));

   // O(1) increment
   for (each entity_id in posting_list) {
       hit_counts[entity_id]++;
   }

   // Collect results with threshold
   for (uint32_t i = 0; i < num_entities; i++) {
       if (hit_counts[i] >= min_hits) {
           add_to_results(i, hit_counts[i]);
       }
   }
   ```

2. **Limit posting list processing:**
   - Skip n-grams with >50K entities (too common, low discriminative value)
   - Process rarest n-grams first (likely to have target entities)

3. **Early termination:**
   - Stop when we have enough high-quality matches
   - Use heap for top-k instead of sorting all

**Files to modify:**
- `lc_ngram.c`: Rewrite `lc_ngram_search()`
- `lc_ngram.h`: Add scratch buffer to index struct

### Phase 2: Fast Binary Loading (2-3 days)

**Goal:** Reduce binary load from 43.4s to <1s

1. **Serialize trie structure:**
   ```c
   // Flatten trie to array with child offsets
   struct TrieNodeCompact {
       uint32_t entity_offset;  // Offset into entity_id array
       uint16_t entity_count;
       uint8_t child_bitmap[5]; // 37 bits for children
       // Followed by child offsets (variable length)
   };
   ```

2. **Serialize spatial grid:**
   ```c
   // Header + cell arrays
   struct GridHeader {
       SHBBox bounds;
       float cell_size;
       uint32_t num_cells_x, num_cells_y;
   };
   // Followed by: cell_offsets[num_cells], entity_ids[total_entities]
   ```

3. **Update binary format:**
   ```
   [Header]
   [Entity Data]     <- existing
   [String Pool]     <- new: all strings concatenated
   [Trie Data]       <- new: serialized trie
   [Grid Data]       <- new: serialized spatial grid
   [N-gram Entries]  <- optional: sorted entries
   ```

4. **mmap() support:**
   - Load file with mmap()
   - Point structures directly at mapped memory
   - Zero-copy for strings

**Files to modify:**
- `lc_serialize.c`: Add trie/grid serialization
- `lc_serialize.h`: Bump version, add format constants
- `lc_trie.c`: Add `lc_trie_serialize()` / `lc_trie_deserialize()`
- `lc_spatial.c`: Add `lc_grid_serialize()` / `lc_grid_deserialize()`

### Phase 3: Memory Optimization (2-3 days)

**Goal:** Reduce memory from 1.9GB to <500MB

1. **String pool with offsets:**
   ```c
   typedef struct {
       char *pool;           // Single allocation
       size_t pool_size;
       size_t pool_used;
   } LCStringPool;

   typedef struct {
       uint32_t name_offset;      // Instead of char *name
       uint32_t street_offset;
       // ...
   } LCEntityCompact;
   ```

2. **Compact entity struct:**
   - Pack fields tightly (current struct has padding)
   - Use bitfields for admin_level, type, fclass
   - Store lat/lon as int32_t (microdegrees) instead of double

3. **Deduplicate strings:**
   - Many entities share street names, city names
   - Hash table to find existing strings

**Files to modify:**
- `lc_types.h`: New compact entity struct
- `lc_types.c`: String pool management
- `lc_pbf.c`: Use string pool during parsing

### Phase 4: Parallel PBF Parsing (1-2 days)

**Goal:** Reduce PBF load from 53.7s to <15s

1. **mmap() the file:**
   ```c
   int fd = open(path, O_RDONLY);
   void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
   ```

2. **Parallel blob processing:**
   ```c
   // First pass: find all blob boundaries (fast, sequential)
   BlobInfo blobs[MAX_BLOBS];
   size_t num_blobs = find_blobs(data, size, blobs);

   // Second pass: process blobs in parallel
   #pragma omp parallel for
   for (size_t i = 0; i < num_blobs; i++) {
       LCEntityStore *local = parse_blob(&blobs[i]);
       // Thread-safe merge into global store
   }
   ```

**Files to modify:**
- `lc_pbf.c`: Add mmap and parallel support

---

## Quick Wins (Can do immediately)

1. **Increase fuzzy threshold:** Change default from 0.3 to 0.5 to reduce candidates

2. **Add n-gram frequency limit:** Skip trigrams with >10K entities in search

3. **Pre-sort posting lists by frequency:** Rarer entities first

4. **Cache normalized queries:** Many repeated queries in real usage

---

## Metrics After Each Phase

| Phase | Fuzzy Search | Binary Load | Memory | PBF Load |
|-------|--------------|-------------|--------|----------|
| Current | 34.8ms | 43.4s | 1.9GB | 53.7s |
| Phase 1 | <100µs | 43.4s | 1.9GB | 53.7s |
| Phase 2 | <100µs | <1s | 1.9GB | 53.7s |
| Phase 3 | <100µs | <1s | <500MB | 53.7s |
| Phase 4 | <100µs | <1s | <500MB | <15s |

---

## Testing Strategy

1. **Regression tests:** Run existing 52 tests after each change

2. **Performance tests:** Run benchmark before/after each phase

3. **Correctness tests:** Verify search results match before optimization
   ```bash
   ./search ../data/hungary.osm.pbf "Budapest" > before.txt
   # ... make changes ...
   ./search ../data/hungary.osm.pbf "Budapest" > after.txt
   diff before.txt after.txt  # Should match
   ```

4. **Memory tests:** Run with Valgrind to catch leaks

---

## Estimated Impact

| Metric | Before | After | User Impact |
|--------|--------|-------|-------------|
| Cold start | 54s | <2s | Instant API startup |
| Warm start | 43s | <1s | Fast server restart |
| Search latency | 35ms worst | <1ms | Real-time autocomplete |
| Memory/entity | 1.7KB | <500B | 3x more entities in RAM |
| API throughput | ~30 QPS | ~1000 QPS | 30x more users |
