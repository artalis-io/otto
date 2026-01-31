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
│   └── lc_index.h    # Unified geocoding index
├── src/              # Implementation
│   ├── lc_pbf.c      # PBF file parsing (uses shared library)
│   ├── lc_normalize.c# UTF-8 normalization, diacritics
│   ├── lc_trie.c     # Compact prefix trie
│   ├── lc_ngram.c    # Trigram fuzzy matching
│   ├── lc_spatial.c  # Grid spatial index
│   ├── lc_index.c    # Main index operations
│   └── locus.c       # Convenience API
├── api/              # REST API server
├── wasm/             # WebAssembly build
├── ../shared/        # Shared library (protobuf, inflate, PBF, geo)
├── ../vendor/        # Third-party (miniz)
├── tests/            # Test suite (52 tests)
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

## Build Commands

```bash
make all      # Build library + tests
make lib      # Build liblocus.a only
make test     # Run test suite (52 tests)
make bench    # Build and run benchmarks
make debug    # Debug build with symbols
make wasm     # WebAssembly (needs Emscripten)
make clean    # Remove build artifacts
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

| Class | Description | Example |
|-------|-------------|---------|
| `LC_CLASS_PLACE` | Named places | Cities, towns, villages |
| `LC_CLASS_STREET` | Roads | Streets, highways |
| `LC_CLASS_ADDRESS` | House numbers | addr:housenumber |
| `LC_CLASS_POI` | Points of interest | Restaurants, shops |
| `LC_CLASS_ADMIN` | Administrative | Countries, states |
| `LC_CLASS_NATURAL` | Natural features | Parks, lakes |
| `LC_CLASS_TRANSPORT` | Transport | Stations, airports |
| `LC_CLASS_OTHER` | Other | Misc features |

## Testing

```bash
# Run all tests
make test

# Expected: 52 tests pass
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
           entity->name, entity->coord.lat, entity->coord.lon,
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
