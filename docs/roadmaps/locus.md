# Locus Geocoding Roadmap

Development roadmap for Locus geocoding library covering search algorithms, performance optimization, and planned features.

## Status Summary (Feb 2026)

| Area | Status | Tests |
|------|--------|-------|
| **Forward Search** | ✅ Complete | Exact + prefix matching |
| **Autocomplete** | ✅ Complete | Prefix trie |
| **Fuzzy Search** | ✅ Complete | Trigram index |
| **Reverse Geocoding** | ✅ Complete | Grid spatial index |
| **Binary Index** | ✅ Complete | `.lcx` format |
| **REST API** | ✅ Complete | Port 8083 |
| **WASM Build** | ✅ Complete | Browser support |

**Test Count**: 68 tests (100% passing)

---

## Chapter 1: Current Performance

### 1.1 Benchmark Results (Hungary - 1.1M entities)

| Operation | Current | Target | Gap |
|-----------|---------|--------|-----|
| PBF Loading | 53.7s | <10s | 5x |
| Binary Load | 43.4s | <1s | 40x |
| Fuzzy Search | 34.8ms | <100µs | 350x |
| Miss Search | 4.1ms | <10µs | 400x |
| Memory Usage | 1.9GB | <500MB | 4x |
| Exact Search | 2.0µs | ✅ OK | - |
| Reverse Geocode | 24.8µs | ✅ OK | - |

### 1.2 Performance (Monaco - 2763 entities)

| Operation | Latency |
|-----------|---------|
| Forward search | 5-11 µs |
| Autocomplete | 5-19 µs |
| Reverse geocoding | 23-24 µs |

---

## Chapter 2: Critical Bottlenecks

### 2.1 Fuzzy Search - O(n²) Hit Tracking

**Location:** `lc_ngram.c:224-249`

**Problem:** Linear scan to find/add entity hits creates O(n²) for common trigrams:
```c
for (size_t k = 0; k < hits_count; k++) {
    if (hits[k].entity_id == eid) {  // O(n) per hit!
        found = (int)k;
        break;
    }
}
```

**Solutions:**

| Approach | Complexity | Notes |
|----------|------------|-------|
| Hash table for hits | O(1) lookup | Replace linear array |
| Bitmap + count array | O(1) lookup | `uint8_t count[num_entities]` |
| Inverted posting lists | O(k) merge | Pre-sorted entity lists |
| Limit candidate set | O(n) bounded | Cap by posting list length |

**Recommended:** Bitmap approach for O(1) increment per hit.

### 2.2 Binary Index Rebuild

**Problem:** Binary load saves only entity data, then rebuilds all indexes (~80% of PBF parse time).

**Solutions:**

| Phase | Approach | Impact |
|-------|----------|--------|
| 1 | Serialize trie + spatial grid | 10x faster load |
| 2 | mmap() entire index file | <100ms load |

### 2.3 Memory Usage

**Problem:** Each entity has individually malloc'd strings (11M allocations for 1.1M entities).

**Solutions:**
- String pool/arena allocation
- Interned string table
- Compact entity representation

---

## Chapter 3: Planned Features

### 3.1 Search Improvements

| Feature | Priority | Description |
|---------|----------|-------------|
| Address parsing | High | Parse "123 Main St, City" |
| Structured search | High | Separate street/city/zip fields |
| Phonetic matching | Medium | Soundex/Metaphone for typos |
| Boundary containment | Medium | "Restaurants in Budapest" |
| Language preferences | Low | Prefer name:en over name |

### 3.2 Index Optimizations

| Feature | Priority | Impact |
|---------|----------|--------|
| Serialized trie | High | 10x faster binary load |
| mmap() support | High | Instant load, shared memory |
| Compressed strings | Medium | 4x memory reduction |
| Lazy index build | Low | Amortize startup cost |

### 3.3 API Enhancements

| Feature | Priority | Description |
|---------|----------|-------------|
| Batch geocoding | Medium | Multiple queries in one call |
| Confidence scores | Medium | Explain match quality |
| Bounding box filter | Low | Limit results to region |
| Category filter | Low | Only POIs, only streets |

---

## Chapter 4: Data Structures

### 4.1 Entity Store

```c
typedef struct {
    uint64_t osm_id;
    LCEntityType type;
    LCFeatureClass fclass;
    char *name;
    char **alt_names;
    int num_alt_names;
    SHCoord centroid;
    SHBBox bbox;
    int admin_level;
    int population;
    LCAddress address;
} LCEntity;
```

### 4.2 Feature Classes

| Class | Description | Example |
|-------|-------------|---------|
| `LC_CLASS_PLACE` | Named places | Cities, towns |
| `LC_CLASS_STREET` | Roads | Streets, highways |
| `LC_CLASS_ADDRESS` | House numbers | addr:housenumber |
| `LC_CLASS_POI` | Points of interest | Restaurants |
| `LC_CLASS_ADMIN` | Administrative | Countries, states |
| `LC_CLASS_NATURAL` | Natural features | Parks, lakes |
| `LC_CLASS_TRANSPORT` | Transport | Stations, airports |

### 4.3 Index Components

| Component | Purpose | Algorithm |
|-----------|---------|-----------|
| Prefix Trie | Autocomplete | 37-slot alphabet (a-z, 0-9, space) |
| Trigram Index | Fuzzy search | Jaccard similarity |
| Spatial Grid | Reverse geocode | Cell-based lookup |

---

## Related Files

| File | Purpose |
|------|---------|
| `locus/CLAUDE.md` | Development guide, API reference |
| `locus/include/locus.h` | Public API |
| `locus/api/` | REST API server |

## Keel Migration — Phase 6 of 6 (✅ Complete)

**Completed for Locus.** `locus/api` runs on Keel v3, and with it the last OTTO
API server has moved off the legacy HTTP server.

Rationale and shared context: `docs/roadmaps/surge.md` (Phase 1).

### Shape of the port

The most mechanical of the six — same structure as FuelWise, with no static
files, no ETag and no multi-listener model:

- `ShWorkQueue` + `ShWorkerPool` + `ShCompletion` → `KlThreadPool` +
  `KlAsyncOp`, same `GeoCtx` ownership rules and the `on_resume` fix.
- `mg_http_var()` → `sh_query_get_str()`.
- `sh_mg_*` → `sh_http_*`.
- The three direct-execution fallbacks collapsed into `submit_geocode_work()`,
  which runs inline when there is no pool.
- All six endpoints are exact paths, so all six are real routes — which matters,
  because `kl_async_suspend()` is only honoured after a route handler (see
  `docs/roadmaps/carta.md`).

`/api/v1/stats` keeps its `work_queue` shape via `LocusQueueStats`.

### Verification

`locus/api/test_api.sh` starts the server against Monaco and gates CI: health,
stats, search, autocomplete, reverse, missing/invalid parameters, 404, 405,
CORS preflight, and async dispatch under concurrent searches.

Not verified locally: Locus needs `mmap`/`sys/mman.h` (`lc_index.c`,
`lc_serialize.c`), which MinGW lacks, so like Carta this could not be
smoke-tested on Windows first.

### CI structure

With Locus done, every API server has its own standalone gating job, so the
`test-api` job that used to hold them was empty and has been removed. The jobs
no longer depend on `test-c`, which means an unrelated failure there can no
longer silently skip API coverage.

## Legacy HTTP server removal

Completed — all six servers run on Keel v3. The previous GPL HTTP server (its
`.c` wrapper and transport-agnostic header) and the retired vendored HTTP server
have been removed, along with their entries in `docs/ARCHITECTURE.md`'s vendor
table and module `CLAUDE.md` files. The Keel helper layer that replaced it was
later renamed `sh_keelserver`/`sh_keelasync` → `sh_httpserver`/`sh_httpasync`
(see `docs/roadmaps/infrastructure.md`).
