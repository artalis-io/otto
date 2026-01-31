# Locus Implementation Plan

**L**ocation **O**riented **C**oordinate **U**nification **S**ystem

Zero-dependency geocoding library for OSM data. Completes the GIS trifecta:
- **Carta** - Map tile generation
- **Velo** - Routing
- **Locus** - Geocoding

## Directory Structure

```
locus/
├── include/
│   ├── locus.h           # Unified public API
│   ├── lc_types.h        # Data structures
│   ├── lc_pbf.h          # PBF parsing (uses shared/)
│   ├── lc_index.h        # Search index structures
│   ├── lc_search.h       # Forward geocoding
│   └── lc_reverse.h      # Reverse geocoding
├── src/
│   ├── locus.c           # Main API
│   ├── lc_pbf.c          # OSM PBF extraction (names, addresses, boundaries)
│   ├── lc_trie.c         # Prefix trie for autocomplete
│   ├── lc_ngram.c        # N-gram index for fuzzy matching
│   ├── lc_spatial.c      # R-tree/grid for reverse geocoding
│   ├── lc_search.c       # Forward geocoding logic
│   ├── lc_reverse.c      # Reverse geocoding logic
│   ├── lc_rank.c         # Result ranking/scoring
│   └── lc_normalize.c    # Text normalization (lowercase, diacritics)
├── api/                  # REST API (mongoose)
│   ├── src/main.c
│   └── Makefile
├── wasm/                 # WebAssembly build
├── tests/
└── benchmarks/
```

---

## Phase 1: Core Data Structures and PBF Extraction

### 1.1 Define Core Types (`lc_types.h`)

- [ ] `LCStatus` enum (LC_OK, LC_ERROR_*, etc.)
- [ ] `LCEntityType` enum (LC_ENTITY_NODE, LC_ENTITY_WAY, LC_ENTITY_RELATION)
- [ ] `LCFeatureClass` enum:
  - LC_CLASS_COUNTRY
  - LC_CLASS_STATE
  - LC_CLASS_CITY
  - LC_CLASS_TOWN
  - LC_CLASS_VILLAGE
  - LC_CLASS_SUBURB
  - LC_CLASS_NEIGHBOURHOOD
  - LC_CLASS_STREET
  - LC_CLASS_ADDRESS
  - LC_CLASS_POI
  - LC_CLASS_OTHER
- [ ] `LCAddress` struct (housenumber, street, city, postcode, state, country)
- [ ] `LCEntity` struct:
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
      int admin_level;        // 0 if not a boundary
      int population;         // 0 if unknown
      LCAddress address;
  } LCEntity;
  ```

### 1.2 PBF Extraction (`lc_pbf.c`)

- [ ] `LCPBFContext` struct to hold extraction state
- [ ] `lc_pbf_context_create()` / `lc_pbf_context_free()`
- [ ] `lc_pbf_parse_file()` - extract geocodable entities
- [ ] Extract nodes with:
  - `place=*` (city, town, village, hamlet, suburb, neighbourhood, locality)
  - `name=*` + amenity/shop/tourism/etc (POIs)
  - `addr:housenumber` + `addr:street` (address points)
- [ ] Extract ways with:
  - `highway=*` + `name=*` (streets)
  - `place=*` (place areas)
  - `building=*` + `addr:*` (building addresses)
- [ ] Extract relations with:
  - `boundary=administrative` + `admin_level=*`
  - `place=*` (place boundaries)
- [ ] Compute centroids for ways/relations
- [ ] Extract alternative names: `name:*`, `alt_name`, `old_name`, `short_name`
- [ ] Parse `population=*` tag for places

### 1.3 Entity Storage

- [ ] `LCEntityStore` - dynamic array of entities
- [ ] String interning for names (reduce memory)
- [ ] `lc_entity_store_add()` / `lc_entity_store_get()`

### 1.4 Tests

- [ ] Test entity type detection from tags
- [ ] Test feature class detection
- [ ] Test centroid computation
- [ ] Test address parsing
- [ ] Test name extraction with alternatives

---

## Phase 2: Text Normalization

### 2.1 Normalization Functions (`lc_normalize.c`)

- [ ] `lc_normalize()` - full normalization pipeline
- [ ] `lc_lowercase()` - case folding (handle UTF-8)
- [ ] `lc_remove_diacritics()` - á→a, ö→o, ñ→n, etc.
- [ ] `lc_normalize_whitespace()` - collapse multiple spaces, trim
- [ ] `lc_remove_punctuation()` - remove non-alphanumeric (keep spaces)
- [ ] Common abbreviation expansion: "st" → "street", "ave" → "avenue"

### 2.2 Unicode Handling

- [ ] UTF-8 aware lowercase (at minimum: Latin, Cyrillic, Greek)
- [ ] Diacritic mapping table (precomposed → base character)
- [ ] Handle German ß → ss, Turkish ı → i

### 2.3 Tests

- [ ] Test lowercase: "BUDAPEST" → "budapest"
- [ ] Test diacritics: "Zürich" → "zurich", "Kraków" → "krakow"
- [ ] Test mixed: "São Paulo" → "sao paulo"
- [ ] Test whitespace: "  New   York  " → "new york"

---

## Phase 3: Prefix Trie for Autocomplete

### 3.1 Trie Structure (`lc_trie.c`)

- [ ] `LCTrieNode` struct:
  ```c
  typedef struct LCTrieNode {
      struct LCTrieNode *children[37]; // a-z (26) + 0-9 (10) + space (1)
      uint32_t *entity_ids;
      uint16_t num_entities;
      uint16_t capacity;
  } LCTrieNode;
  ```
- [ ] `LCTrie` struct (root node + stats)
- [ ] `lc_trie_create()` / `lc_trie_free()`
- [ ] `lc_trie_insert(trie, normalized_name, entity_id)`
- [ ] `lc_trie_search_prefix(trie, prefix, max_results, results)`
- [ ] `lc_trie_search_exact(trie, name, results)`

### 3.2 Memory Optimization

- [ ] Lazy child allocation (NULL until needed)
- [ ] Compact entity_id arrays (realloc as needed)
- [ ] Consider path compression for sparse branches

### 3.3 Tests

- [ ] Test single insertion and retrieval
- [ ] Test prefix search: "buda" → ["budapest", "budaörs", ...]
- [ ] Test exact match
- [ ] Test empty prefix (all entities)
- [ ] Test no match

---

## Phase 4: N-gram Index for Fuzzy Matching

### 4.1 N-gram Index (`lc_ngram.c`)

- [ ] `LCNgramIndex` struct:
  ```c
  typedef struct {
      LCNgramEntry *entries;  // sorted by ngram
      uint32_t num_entries;
      uint32_t capacity;
  } LCNgramIndex;

  typedef struct {
      char ngram[4];          // 3-gram + null
      uint32_t *entity_ids;
      uint32_t count;
      uint32_t capacity;
  } LCNgramEntry;
  ```
- [ ] `lc_ngram_create()` / `lc_ngram_free()`
- [ ] `lc_ngram_index_name(idx, normalized_name, entity_id)`
- [ ] `lc_ngram_search(idx, query, threshold, results)`

### 4.2 Fuzzy Matching Algorithm

- [ ] Generate 3-grams from query
- [ ] Find entities sharing N% of query n-grams (Jaccard similarity)
- [ ] Rank by similarity score
- [ ] Optional: Levenshtein distance for top candidates

### 4.3 Tests

- [ ] Test n-gram generation: "budapest" → ["bud", "uda", "dap", "ape", "pes", "est"]
- [ ] Test fuzzy match: "budapset" → "budapest"
- [ ] Test similarity scoring

---

## Phase 5: Spatial Index for Reverse Geocoding

### 5.1 Grid Index (`lc_spatial.c`)

- [ ] `LCSpatialGrid` struct:
  ```c
  typedef struct {
      double cell_size_lat;
      double cell_size_lon;
      int grid_width;
      int grid_height;
      SHBBox bounds;
      uint32_t **cells;       // entity_id lists per cell
      uint32_t *cell_counts;
      uint32_t *cell_capacities;
  } LCSpatialGrid;
  ```
- [ ] `lc_grid_create(bbox, cell_size)`
- [ ] `lc_grid_insert(grid, coord, entity_id)`
- [ ] `lc_grid_query_point(grid, coord, results)`
- [ ] `lc_grid_query_radius(grid, coord, radius_m, results)`

### 5.2 R-tree for Boundaries (optional, Phase 5b)

- [ ] `LCRTreeNode` struct for polygon containment
- [ ] `lc_rtree_create()` / `lc_rtree_free()`
- [ ] `lc_rtree_insert(rtree, bbox, entity_id)`
- [ ] `lc_rtree_query_point(rtree, coord, results)` - find containing polygons

### 5.3 Point-in-Polygon (for boundaries)

- [ ] Store boundary polygons (simplified)
- [ ] Ray casting algorithm for containment test
- [ ] Cache boundary polygons by admin_level

### 5.4 Tests

- [ ] Test grid cell calculation
- [ ] Test point query
- [ ] Test radius query
- [ ] Test boundary containment

---

## Phase 6: Forward Geocoding

### 6.1 Search API (`lc_search.c`)

- [ ] `LCSearchOptions` struct:
  ```c
  typedef struct {
      int limit;              // Max results (default: 10)
      SHBBox *bounds;         // Optional geographic filter
      LCFeatureClass *filter; // Optional class filter (array, NULL-terminated)
      const char *lang;       // Preferred language (e.g., "en")
      int fuzzy;              // Enable fuzzy matching (default: 1)
  } LCSearchOptions;
  ```
- [ ] `LCSearchResult` struct:
  ```c
  typedef struct {
      LCEntity **results;
      double *scores;
      int num_results;
  } LCSearchResult;
  ```
- [ ] `lc_search(index, query, options, result)`
- [ ] `lc_autocomplete(index, prefix, limit, result)`
- [ ] `lc_search_result_free(result)`

### 6.2 Search Algorithm

- [ ] Normalize query
- [ ] Trie prefix search
- [ ] If insufficient results: n-gram fuzzy search
- [ ] Apply geographic filter (if bounds specified)
- [ ] Apply class filter (if specified)
- [ ] Rank and sort results
- [ ] Return top N

### 6.3 Ranking (`lc_rank.c`)

- [ ] `lc_rank_results(query, results, count)` - compute scores
- [ ] Scoring factors:
  - Exact match: +1.0
  - Prefix match: +0.8
  - Fuzzy match: 0.5-0.7 (by similarity)
  - Feature importance: country(+0.3) > city(+0.2) > street(+0.1) > POI(+0.0)
  - Population bonus: log10(population) * 0.05
  - Name length penalty: -0.01 * len (prefer shorter names)

### 6.4 Tests

- [ ] Test exact search: "Budapest" → Budapest
- [ ] Test prefix search: "Buda" → Budapest, Budaörs, ...
- [ ] Test fuzzy search: "Budapset" → Budapest
- [ ] Test with bounds filter
- [ ] Test with class filter
- [ ] Test ranking order

---

## Phase 7: Reverse Geocoding

### 7.1 Reverse API (`lc_reverse.c`)

- [ ] `LCReverseOptions` struct:
  ```c
  typedef struct {
      int include_poi;        // Include nearest POI (default: 0)
      double radius_m;        // Search radius (default: 100.0)
      const char *lang;       // Preferred language
  } LCReverseOptions;
  ```
- [ ] `LCReverseResult` struct:
  ```c
  typedef struct {
      LCAddress address;      // Structured address
      LCEntity *place;        // Nearest named place
      LCEntity *street;       // Nearest street
      LCEntity *poi;          // Nearest POI (if requested)
      LCEntity **hierarchy;   // Admin hierarchy [country, state, city, ...]
      int hierarchy_depth;
      double distance_m;      // Distance to nearest feature
  } LCReverseResult;
  ```
- [ ] `lc_reverse(index, coord, options, result)`
- [ ] `lc_reverse_result_free(result)`

### 7.2 Reverse Algorithm

- [ ] Query spatial grid for nearby entities
- [ ] Find nearest street (for address)
- [ ] Find nearest address point (if any)
- [ ] Find containing administrative boundaries (by level)
- [ ] Build hierarchy: country → state → city → suburb → neighbourhood
- [ ] Interpolate house number if on street segment
- [ ] Optionally find nearest POI

### 7.3 Address Formatting

- [ ] `lc_format_address(result, format, buffer, size)`
- [ ] Format templates by country (future)
- [ ] Default: "{housenumber} {street}, {city}, {country}"

### 7.4 Tests

- [ ] Test reverse in city center → city name
- [ ] Test reverse on street → street name
- [ ] Test reverse near address → full address
- [ ] Test hierarchy building
- [ ] Test with POI inclusion

---

## Phase 8: Unified Index

### 8.1 Index Structure (`lc_index.h`)

- [ ] `LCIndex` struct:
  ```c
  typedef struct {
      LCEntityStore *entities;
      LCTrie *trie;
      LCNgramIndex *ngrams;
      LCSpatialGrid *grid;
      LCRTree *boundaries;    // Admin boundaries
      SHBBox bounds;
      uint32_t num_entities;
  } LCIndex;
  ```
- [ ] `lc_index_create()` / `lc_index_free()`
- [ ] `lc_index_build_from_pbf(path)` - full pipeline

### 8.2 Binary Index Format

- [ ] Magic: "LCIX" (4 bytes)
- [ ] Version: uint32
- [ ] Header: bounds, counts, offsets
- [ ] Sections: entities, trie, ngrams, grid, boundaries
- [ ] `lc_index_save(index, path)`
- [ ] `lc_index_load(path)` - mmap for speed

### 8.3 Tests

- [ ] Test build from PBF
- [ ] Test save/load roundtrip
- [ ] Test mmap load performance

---

## Phase 9: Main API (`locus.c`, `locus.h`)

### 9.1 Public API

- [ ] `lc_version()` - version string
- [ ] `lc_status_string(status)` - error messages
- [ ] `lc_load_pbf(path)` - load and build index
- [ ] `lc_load_index(path)` - load binary index
- [ ] `lc_save_index(index, path)` - save binary index
- [ ] `lc_search(index, query, opts, result)`
- [ ] `lc_autocomplete(index, prefix, limit, result)`
- [ ] `lc_reverse(index, coord, opts, result)`
- [ ] `lc_free_index(index)`
- [ ] `lc_free_search_result(result)`
- [ ] `lc_free_reverse_result(result)`

### 9.2 Default Options

- [ ] `lc_default_search_options(opts)`
- [ ] `lc_default_reverse_options(opts)`

---

## Phase 10: Build System

### 10.1 Makefile

- [ ] `make all` - build library + tests
- [ ] `make lib` - build liblocus.a
- [ ] `make test` - run tests
- [ ] `make bench` - run benchmarks
- [ ] `make debug` - debug build
- [ ] `make wasm` - WebAssembly build
- [ ] `make clean`
- [ ] Link against shared library (libshared.a)
- [ ] Link against miniz

### 10.2 Integration

- [ ] Add to root Makefile
- [ ] Add `make locus` target
- [ ] Add `make test-locus` target

---

## Phase 11: REST API

### 11.1 Endpoints

- [ ] `GET /api/v1/health` - health check
- [ ] `GET /api/v1/stats` - index statistics
- [ ] `GET /api/v1/search?q=<query>&limit=<n>&bounds=<bbox>` - forward geocoding
- [ ] `GET /api/v1/autocomplete?q=<prefix>&limit=<n>` - autocomplete
- [ ] `GET /api/v1/reverse?lat=<lat>&lon=<lon>` - reverse geocoding

### 11.2 Response Format

```json
{
  "results": [
    {
      "osm_id": 21335,
      "osm_type": "relation",
      "name": "Budapest",
      "display_name": "Budapest, Central Hungary, Hungary",
      "class": "place",
      "type": "city",
      "lat": 47.497912,
      "lon": 19.040235,
      "bbox": [18.926, 47.350, 19.335, 47.613],
      "address": {
        "city": "Budapest",
        "state": "Central Hungary",
        "country": "Hungary",
        "country_code": "hu"
      },
      "score": 0.98
    }
  ],
  "query": "Budapest",
  "took_ms": 2.3
}
```

### 11.3 Implementation

- [ ] Mongoose HTTP server setup
- [ ] Request parsing (query params)
- [ ] JSON response formatting
- [ ] Error handling
- [ ] CORS headers

### 11.4 Makefile

- [ ] `make locus-api` - build API server
- [ ] `make run-locus-api` - run server
- [ ] `make test-locus-api` - integration tests

---

## Phase 12: WASM Build

- [ ] Emscripten build configuration
- [ ] Export functions: lc_search, lc_autocomplete, lc_reverse
- [ ] Memory management for WASM
- [ ] TypeScript declarations
- [ ] Example browser usage

---

## Phase 13: Documentation

- [ ] README.md - overview, quick start, examples
- [ ] CLAUDE.md - developer guide, architecture
- [ ] API documentation in header comments
- [ ] Update root CLAUDE.md with locus info

---

## Performance Targets

| Operation | Target |
|-----------|--------|
| Index build (Hungary, ~35M OSM objects) | < 60s |
| Binary index load (mmap) | < 500ms |
| Forward search | < 10ms |
| Autocomplete | < 5ms |
| Reverse geocoding | < 5ms |

## Memory Estimates (Hungary)

| Component | Estimated Size |
|-----------|----------------|
| Entity data | ~200 MB |
| Text trie | ~50 MB |
| N-gram index | ~100 MB |
| Spatial grid | ~20 MB |
| Boundaries | ~30 MB |
| **Total** | ~400 MB |

---

## Milestones

1. **M1**: PBF extraction working, entities stored (Phase 1)
2. **M2**: Text search working (Phases 2-4)
3. **M3**: Forward geocoding complete (Phase 6)
4. **M4**: Reverse geocoding complete (Phases 5, 7)
5. **M5**: Binary index save/load (Phase 8)
6. **M6**: API server working (Phase 11)
7. **M7**: WASM build (Phase 12)
8. **M8**: Documentation complete (Phase 13)
