# Claude Code Instructions for Locus

## Project Overview

Locus is a zero-dependency geocoding library that parses OSM PBF files and provides forward geocoding (text search), autocomplete, and reverse geocoding (coordinate to address).

## Quick Start

```bash
# Build and test
make all && make test

# Run benchmarks
make bench

# Run API server
make -C api && ./api/locus-geocoder ../data/monaco-latest.osm.pbf
```

## Directory Structure

```
locus/
├── include/          # Public headers
│   ├── locus.h       # Unified API
│   ├── lc_types.h    # Data structures (LCEntity, LCFeatureClass)
│   ├── lc_pbf.h      # PBF parsing
│   ├── lc_normalize.h# Text normalization
│   ├── lc_trie.h     # Prefix trie for autocomplete
│   ├── lc_ngram.h    # Trigram index for fuzzy search
│   ├── lc_spatial.h  # Grid-based spatial index
│   ├── lc_index.h    # Unified geocoding index
│   ├── lc_query.h    # Query parsing (housenumber splitting)
│   ├── lc_serialize.h# Index serialization
│   ├── lc_mmap.h     # Zero-copy mmap'd index (v3/v4)
│   └── lc_api.h      # REST handler, transport-agnostic
├── src/              # Implementation
│   ├── lc_pbf.c      # PBF file parsing (uses shared library)
│   ├── lc_normalize.c# UTF-8 normalization, diacritics
│   ├── lc_trie.c     # Compact prefix trie
│   ├── lc_ngram.c    # Trigram fuzzy matching
│   ├── lc_spatial.c  # Grid spatial index
│   ├── lc_index.c    # Main index operations
│   ├── lc_types.c    # Feature-class helpers
│   ├── lc_query.c    # Query parsing
│   ├── lc_serialize.c# Index read/write
│   ├── lc_api.c      # Request handling and JSON responses
│   └── locus.c       # Convenience API
├── api/              # REST API server (locus-geocoder, port 8083)
├── wasm/             # WebAssembly build
├── tools/            # geocode_batch, search
├── ../shared/        # Shared library (protobuf, inflate, PBF, geo)
├── ../vendor/        # Third-party (miniz)
├── tests/            # Test suite (73 tests) + fuzz harnesses
└── benchmarks/       # Performance tests
```

**Note:** Protobuf decoding and zlib decompression are provided by the shared library (`sh_protobuf.h`, `sh_inflate.h`, `sh_pbf.h`).

## Key Files

| File | Purpose |
|------|---------|
| `lc_types.h` | Entity structure, feature classes |
| `lc_index.h` | Main API (search, autocomplete, reverse) |
| `lc_pbf.c` | OSM PBF parsing (DenseNodes, Ways) |
| `lc_normalize.c` | Unicode normalization, diacritic removal |
| `lc_trie.c` | Prefix trie for fast autocomplete |
| `lc_ngram.c` | Trigram index for fuzzy matching |
| `lc_spatial.c` | Grid-based reverse geocoding |
| `lc_api.c` | REST request handling, JSON responses |

## Build Commands

```bash
make all      # Build library + tests
make lib      # Build liblocus.a only
make test     # Run test suite (73 tests)
make bench    # Build and run benchmarks
make debug    # Debug build with symbols
make wasm     # WebAssembly (needs Emscripten)
make clean    # Remove build artifacts

make fuzz-api        # Build the lc_api_handle() libFuzzer harness (needs clang)
make fuzz-api-smoke  # Bounded run over the committed seeds, as CI does
make fuzz-smoke      # Every harness in the module
```

## Architecture

### Data Flow
```
PBF File → lc_pbf.c → LCEntityStore → lc_index.c → LCIndex
                                           ↓
                                   ┌───────┴───────┐
                                   │               │
                              LCTrie          LCSpatialGrid
                           (autocomplete)     (reverse geocode)
                                   │
                            LCNgramIndex
                            (fuzzy search)
```

### Index Components

1. **Entity Store**: All extracted OSM entities (places, streets, POIs)
2. **Prefix Trie**: Fast autocomplete with 37-slot alphabet (a-z, 0-9, space)
3. **Trigram Index**: Fuzzy matching using Jaccard similarity
4. **Spatial Grid**: Cell-based geographic index for reverse geocoding

### Search Pipeline

```
Query → Normalize → Trie Lookup (exact/prefix)
                         ↓
                   Found matches?
                    ↓         ↓
                   Yes        No
                    ↓         ↓
               Rank/Score  Trigram Fuzzy
                    ↓         ↓
                Results ← Rank/Score
```

### Feature Classes

Listed in priority order, matching `LCFeatureClass` in `lc_types.h`.

| Class | Tagging | Example |
|-------|---------|---------|
| `LC_CLASS_UNKNOWN` | unclassified | — |
| `LC_CLASS_COUNTRY` | `admin_level=2` | France |
| `LC_CLASS_STATE` | `admin_level=4` | Provence-Alpes-Cote d'Azur |
| `LC_CLASS_COUNTY` | `admin_level=6` | Alpes-Maritimes |
| `LC_CLASS_CITY` | `place=city` or `admin_level=8` | Monaco |
| `LC_CLASS_TOWN` | `place=town` | Beausoleil |
| `LC_CLASS_VILLAGE` | `place=village` | Peille |
| `LC_CLASS_SUBURB` | `place=suburb` | Monte Carlo |
| `LC_CLASS_NEIGHBOURHOOD` | `place=neighbourhood` | Larvotto |
| `LC_CLASS_HAMLET` | `place=hamlet` | — |
| `LC_CLASS_LOCALITY` | `place=locality` | — |
| `LC_CLASS_STREET` | `highway=*` with a name | Boulevard Albert 1er |
| `LC_CLASS_ADDRESS` | `addr:housenumber` + `addr:street` | 1 Avenue des Citronniers |
| `LC_CLASS_POI` | `amenity`, `shop`, `tourism`, ... | Cafe de Paris |
| `LC_CLASS_WATER` | `natural=water`, `waterway=*` | Port Hercule |
| `LC_CLASS_OTHER` | other named features | — |

The reverse-geocoding result groups these: `CITY` through `NEIGHBOURHOOD` fill
`result->place`, `STREET` fills `result->street`, `ADDRESS` fills
`result->address`, and `POI` fills `result->poi` when `include_poi` is set.

## Testing

```bash
# Run all tests
make test

# Expected: 73 tests pass
```

### Test Categories
- **Normalization Tests**: Case folding, diacritics, whitespace
- **Trie Tests**: Insert, search, prefix matching
- **N-gram Tests**: Trigram extraction, similarity
- **Spatial Tests**: Grid operations, nearest neighbor
- **Index Tests**: Full search pipeline, reverse geocoding

## Benchmarks

```bash
# Run geocoding benchmarks
./benchmarks/bench_locus ../data/monaco-latest.osm.pbf
```

### Performance (Monaco dataset, 2763 entities)
| Operation | Latency |
|-----------|---------|
| Forward search | 5-11 µs/query |
| Autocomplete | 5-19 µs/query |
| Reverse geocoding | 23-24 µs/query |

## API Usage

### Basic Search

```c
#include "locus.h"

// Create and load index
LCIndex *index = lc_index_create();
lc_index_build_from_pbf(index, "map.osm.pbf", NULL);

// Forward geocoding
LCSearchOptions opts;
lc_search_options_default(&opts);
opts.limit = 10;
opts.fuzzy = 1;

LCSearchResult result;
lc_search(index, "Budapest", &opts, &result);

for (size_t i = 0; i < result.num_results; i++) {
    const LCEntity *entity = lc_search_get_entity(index, &result.matches[i]);
    printf("%s: %.4f, %.4f (score: %.2f)\n",
           entity->name, entity->centroid.lat, entity->centroid.lon,
           result.matches[i].score);
}
lc_search_result_free(&result);

// Cleanup
lc_index_free(index);
```

### Autocomplete

```c
LCSearchResult result;
lc_autocomplete(index, "Bud", 10, &result);

for (size_t i = 0; i < result.num_results; i++) {
    const LCEntity *entity = lc_search_get_entity(index, &result.matches[i]);
    printf("%s\n", entity->name);
}
lc_search_result_free(&result);
```

### Reverse Geocoding

```c
SHCoord coord = {.lat = 47.4979, .lon = 19.0402};

LCReverseOptions opts;
lc_reverse_options_default(&opts);
opts.radius_m = 100.0;

LCReverseResult result;
lc_reverse(index, coord, &opts, &result);

if (result.street) {
    printf("Street: %s\n", result.street->name);
}
if (result.place) {
    printf("Place: %s\n", result.place->name);
}

lc_reverse_result_free(&result);
```

## REST API (api/)

Server runs on port 8083 by default.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Index statistics |
| `/api/v1/search` | GET | Forward geocoding |
| `/api/v1/autocomplete` | GET | Autocomplete |
| `/api/v1/reverse` | GET | Reverse geocoding |

### Example Requests

```bash
# Health check
curl http://localhost:8083/api/v1/health

# Search
curl "http://localhost:8083/api/v1/search?q=Monte%20Carlo&limit=5"

# Autocomplete
curl "http://localhost:8083/api/v1/autocomplete?q=Mon&limit=10"

# Reverse geocoding
curl "http://localhost:8083/api/v1/reverse?lat=43.7384&lon=7.4246"
```

## Common Issues

1. **miniz compilation**: Ensure vendor/ has all miniz files
2. **UTF-8 queries**: All text should be UTF-8 encoded
3. **Memory usage**: Large PBF files need significant memory for indexing
4. **Empty results**: Check if query contains unsupported characters

## Performance Notes

- Text normalization is done once during indexing
- Trie provides O(m) lookup where m = query length
- Trigram matching requires sorting after index build
- Spatial grid cell size affects reverse geocoding accuracy vs speed
- Recommended cell size: 0.01° (~1km at equator)

## Dependencies

- **External**: None
- **Shared**: ../shared (protobuf, inflate, PBF parsing, geo utilities)
- **Vendored**: ../vendor/miniz (public domain zlib, used via shared library)
- **Standard Library**: stdio, stdlib, string, math
