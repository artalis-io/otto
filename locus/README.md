# Locus - OSM Geocoding Library

Zero-dependency geocoding library for OpenStreetMap data. Provides forward geocoding (text search), autocomplete, and reverse geocoding.

## Quick Start

```bash
# Build library and tests
make all

# Run tests
make test

# Build and run API server
make -C api && ./api/locus-geocoder data/monaco-latest.osm.pbf
```

## Features

- **Forward Geocoding**: Search by name with fuzzy matching
- **Autocomplete**: Prefix-based search for type-ahead
- **Reverse Geocoding**: Coordinate to address lookup
- **Binary Index**: Save/load preprocessed indexes (.lcix format)
- **Zero Dependencies**: Only standard C library
- **WASM Support**: Compile to WebAssembly for browser use

## API Endpoints

The API server runs on port 8083 by default.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Index statistics |
| `/api/v1/search?q=<query>` | GET | Forward geocoding |
| `/api/v1/autocomplete?q=<prefix>` | GET | Prefix search |
| `/api/v1/reverse?lat=<lat>&lon=<lon>` | GET | Reverse geocoding |

## Example Usage

```c
#include "locus.h"

// Create and load index
LCIndex *index = lc_index_create();
lc_index_build_from_pbf(index, "map.osm.pbf", NULL);

// Forward search
LCSearchOptions opts;
lc_search_options_default(&opts);
opts.limit = 10;

LCSearchResult result;
lc_search(index, "Budapest", &opts, &result);

for (size_t i = 0; i < result.num_results; i++) {
    const LCEntity *e = lc_search_get_entity(index, &result.matches[i]);
    printf("%s: %.4f, %.4f\n", e->name, e->centroid.lat, e->centroid.lon);
}
lc_search_result_free(&result);

// Reverse geocoding
SHCoord coord = {.lat = 47.4979, .lon = 19.0402};
LCReverseResult rev;
lc_reverse(index, coord, NULL, &rev);
if (rev.place) printf("Place: %s\n", rev.place->name);
lc_reverse_result_free(&rev);

// Cleanup
lc_index_free(index);
```

## Performance

Tested with Monaco dataset (2,763 entities):

| Operation | Latency |
|-----------|---------|
| Forward search | 5-11 µs |
| Autocomplete | 5-19 µs |
| Reverse geocoding | 23-24 µs |
| PBF loading | 86 ms |

Tested with Hungary dataset (~1.1M entities):

| Operation | Latency |
|-----------|---------|
| Forward search | ~10 µs |
| PBF loading | ~15 sec |
| Memory usage | ~1.5 GB |

## Binary Index (.lcix)

For faster startup in production, convert PBF files to binary index format:

```bash
# Build search tool
gcc -O3 tools/search.c -Iinclude -L. -llocus -L../shared -lshared -lm -o search

# Convert PBF to binary index
./search --save data/hungary.osm.pbf data/hungary.lcix

# Search using binary index (faster loading)
./search data/hungary.lcix "Budapest"

# Show file info
./search --info data/hungary.lcix
```

Benefits:
- Smaller file size (~70% reduction)
- Simpler format (no protobuf dependency)
- All entity data preserved with strings

## Architecture

```
PBF File → lc_pbf.c → LCEntityStore → lc_index.c → LCIndex
                            ↓                           ↓
                     lc_serialize.c              ┌──────┴──────┐
                       ↓      ↑                  │             │
                   .lcix file               LCTrie      LCSpatialGrid
                   (binary)              (autocomplete) (reverse geocode)
                                               │
                                         LCNgramIndex
                                         (fuzzy search)
```

## Build Targets

```bash
make all      # Build library + tests
make lib      # Build liblocus.a only
make test     # Run test suite (52 tests)
make bench    # Run benchmarks
make wasm     # WebAssembly build (requires Emscripten)
make clean    # Clean build artifacts
```

## Directory Structure

```
locus/
├── include/          # Public headers
├── src/              # Implementation
├── api/              # REST API server
├── wasm/             # WebAssembly build
├── tests/            # Test suite
├── benchmarks/       # Performance tests
└── tools/            # CLI utilities
```

## License

Part of the OTTO platform. See root LICENSE file.
