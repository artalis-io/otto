# Claude Code Instructions for OTTO Platform

## Project Overview

**OTTO** (**O**ptimization for **T**rucking and **T**ransport **O**perations) is a comprehensive trucking and logistics optimization platform written primarily in C with TypeScript React frontends. It combines route planning, fuel cost optimization, Hours of Service compliance, fleet scheduling, and custom map rendering. All core libraries are zero-dependency and compile to WebAssembly.

## Quick Start

```bash
# Build all libraries
make all

# Run all tests (190+ across all modules)
make test

# Start servers
make run-fuelwise-api         # FuelWise API on :8080
make run-velo-api             # Route server on :8082
make run-carta-api            # Tile server on :8081
make fuelwise-ui-dev          # FuelWise UI on :5173
```

## Component Summary

### Active Components

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| Ralph | `ralph/` | C | LP/MIP solver engine |
| Velo | `velo/` | C | OSM routing engine |
| Carta | `carta/` | C | Map tile generator (MVT/PNG) |
| Locus | `locus/` | C | OSM geocoding engine |
| FuelWise | `fuelwise/` | C | Refueling domain logic |
| ClayShards | `clayshards/` | C+JS | Immediate mode UI components, WebGL renderer |
| Shared | `shared/` | C | Common geo utilities, protobuf, zlib |
| Vendor | `vendor/` | C | Third-party libs (see below) |

### Planned Components

| Component | Location | Purpose |
|-----------|----------|---------|
| Forge | `forge/` | **F**lexible **O**rchestration and **R**untime for **G**eneral **E**xecution - async job queue |
| Nexus | `nexus/` | **N**ormalized **Ex**ternal **U**nified **S**napshots - TMS/ELD/LoadBoard integration gateway |
| HoSE | `hose/` | **H**ours **o**f **S**ervice **E**ngine - FMCSA/EC561 compliance |
| Tempo | `tempo/` | **T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator |
| Arbor | `arbor/` | **A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime |
| Sigma | `sigma/` | **S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation |
| Pulse | `pulse/` | **P**lan **U**tilization and **L**ive **S**tate **E**stimator |

See `docs/TODO_FEATURES.md` for detailed specifications of planned components.
See `docs/NEXUS.md` for the data ingress architecture (TMS/ELD/LoadBoard integration).
See `docs/FORGE.md` for the async job queue architecture.

### Applications

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| FuelWise API | `fuelwise/api/` | C | Refueling REST API |
| Route Server | `velo/api/` | C | Routing REST API |
| Tile Server | `carta/api/` | C | Tile server REST API |
| Geocoding Server | `locus/api/` | C | Geocoding REST API |
| FuelWise WASM | `fuelwise/wasm/` | C+JS | Browser builds |
| Locus WASM | `locus/wasm/` | C+JS | Geocoding browser builds |
| FuelWise UI | `fuelwise/ui/` | TypeScript | React frontend |
| Carta UI | `carta/ui/` | TypeScript | Tile viewer |

## Key Files by Task

### Working on LP/MIP optimization:
- `ralph/include/ralph.h` - Solver API
- `ralph/src/simplex.c` - Revised Simplex algorithm
- `ralph/src/lu.c` - LU factorization (critical)
- `ralph/src/branch_bound.c` - MIP solver

### Working on routing:
- `velo/include/velo.h` - Routing API
- `velo/src/vl_route.c` - Dijkstra, A*, bidirectional
- `velo/src/vl_pbf.c` - OSM PBF parsing
- `velo/src/vl_graph.c` - CSR graph structure

### Working on map tiles:
- `carta/include/carta.h` - Tile generation API
- `carta/src/ct_mvt.c` - MVT encoding
- `carta/src/ct_render.c` - PNG rasterization
- `carta/src/ct_tile.c` - Web Mercator math

### Working on geocoding:
- `locus/include/locus.h` - Geocoding API
- `locus/src/lc_pbf.c` - OSM PBF parsing for places/POIs
- `locus/src/lc_trie.c` - Prefix trie for autocomplete
- `locus/src/lc_ngram.c` - Trigram index for fuzzy search
- `locus/src/lc_spatial.c` - Grid-based reverse geocoding
- `locus/src/lc_index.c` - Main search index

### Working on refueling:
- `fuelwise/include/fuelwise.h` - Library API
- `fuelwise/src/fw_refuel.c` - LP/MILP formulation
- `fuelwise/src/fw_route.c` - Station filtering

### Working on geo utilities (shared):
- `shared/include/shared.h` - Shared API
- `shared/include/sh_geo.h` - Coordinate types and functions
- `shared/src/sh_geo.c` - Haversine, Web Mercator

### Working on PBF/Protobuf parsing (shared):
- `shared/include/sh_protobuf.h` - Protobuf read/write primitives
- `shared/include/sh_inflate.h` - Zlib compress/decompress
- `shared/include/sh_pbf.h` - PBF blob parsing, string tables
- `shared/src/sh_protobuf.c` - Varint, svarint, tags, packed arrays
- `shared/src/sh_inflate.c` - miniz wrapper for inflate/deflate
- `shared/src/sh_pbf.c` - Blob header parsing, decompression

### Working on FuelWise API:
- `fuelwise/api/src/main.c` - HTTP handlers
- `fuelwise/api/CLAUDE.md` - API documentation

### Working on routing API:
- `velo/api/src/main.c` - HTTP handlers
- `velo/api/src/polyline.c` - Google Polyline encoding
- `velo/api/CLAUDE.md` - API documentation

### Working on tile server:
- `carta/api/src/main.c` - HTTP handlers
- `carta/api/CLAUDE.md` - API documentation

### Working on FuelWise UI:
- `fuelwise/ui/src/App.tsx` - Main component
- `fuelwise/ui/src/components/MapView.tsx` - Map integration

### Working on Carta UI:
- `carta/ui/src/App.tsx` - Tile viewer component
- `carta/ui/src/App.css` - Styling

## Build Commands

```bash
# Libraries
make all              # Build all with tests (default)
make lib              # Build libraries only (no tests)
make ralph            # LP/MIP solver
make fuelwise         # Refueling library
make shared           # Shared geo utilities
make velo             # Routing engine
make carta            # Tile generator
make locus            # Geocoding engine

# API Servers
make fuelwise-api     # FuelWise REST API (fuelwise/api)
make velo-api         # Velo route server (velo/api)
make carta-api        # Carta tile server (carta/api)
make locus-api        # Locus geocoding server (locus/api)

# WebAssembly (requires Emscripten)
make wasm             # Build all WASM modules
make wasm-fuelwise    # FuelWise WASM only
make wasm-velo        # Velo WASM only
make wasm-carta       # Carta WASM only
make wasm-locus       # Locus WASM only
make wasm-types       # Generate TypeScript declarations
make wasm-test        # Test WASM builds

# UI (requires Node.js)
make fuelwise-ui      # Build FuelWise UI
make fuelwise-ui-dev  # Run FuelWise UI dev server
make carta-ui         # Build Carta Tile Viewer
make carta-ui-dev     # Run Carta UI dev server

# Testing
make test             # All library tests (~190 tests)
make test-ralph       # 65 tests
make test-fuelwise    # 32 tests
make test-shared      # 23 tests
make test-velo        # 39 tests
make test-carta       # 33 tests
make test-locus       # 52 tests
make test-api         # All API tests (requires OSM data)
make test-fuelwise-api# FuelWise API tests
make test-velo-api    # Velo API tests
make test-carta-api   # Carta API tests
make test-locus-api   # Locus API tests

# Scripts
make benchmark        # Run performance benchmarks
make ci               # Run CI pipeline

# Run servers
make run-fuelwise-api # Start FuelWise API on :8080
make run-velo-api     # Show Velo route server usage
make run-carta-api    # Show Carta tile server usage
make run-locus-api    # Show Locus geocoding server usage
```

## API Endpoints

### FuelWise API (:8080)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/filter` | POST | Filter stations to route |
| `/api/v1/solve` | POST | Solve refueling problem |
| `/api/v1/optimize` | POST | Full pipeline |

### Velo Route Server (:8082)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Graph statistics |
| `/api/v1/route` | GET/POST | Calculate route (profile, mode, geometry) |

### Carta Tile Server (:8081)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | PBF statistics |
| `/tiles/{z}/{x}/{y}.png` | GET | Raster tile (PNG) |
| `/tiles/{z}/{x}/{y}.mvt` | GET | Vector tile (MVT) |
| `/tiles.json` | GET | TileJSON metadata |

### Locus Geocoding Server (:8083)

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Index statistics |
| `/api/v1/search` | GET | Forward geocoding (text to coordinates) |
| `/api/v1/autocomplete` | GET | Prefix-based autocomplete |
| `/api/v1/reverse` | GET | Reverse geocoding (coordinates to address) |

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  UI (React) / WASM (Browser) / API (mongoose)                    │
├──────────────────────────────────────────────────────────────────┤
│  Fleet Optimization [PLANNED]                                     │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌───────┐           │
│  │  HoSE  │ │ Tempo  │ │ Arbor  │ │ Sigma  │ │ Pulse │           │
│  │  HoS   │ │ Time   │ │ Search │ │ Fleet  │ │ PTA   │           │
│  └────────┘ └────────┘ └────────┘ └────────┘ └───────┘           │
├──────────────────────────────────────────────────────────────────┤
│  Domain Libraries                                                 │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐         │
│  │ FuelWise  │ │   Velo    │ │  Carta    │ │  Locus    │         │
│  │ Refueling │ │  Routing  │ │  Tiles    │ │ Geocoding │         │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘         │
│        │             │             │             │                │
│  ┌─────┴─────┐       └──────┬──────┴─────────────┘                │
│  │   Ralph   │        ┌─────┴─────┐                               │
│  │  LP/MIP   │        │  shared   │                               │
│  │  Solver   │        │geo,protobuf                               │
│  └───────────┘        │inflate,pbf│                               │
│                       └─────┬─────┘                               │
│                             │                                     │
│                       ┌─────┴─────┐                               │
│                       │  vendor   │                               │
│                       │  (miniz)  │                               │
│                       └───────────┘                               │
└──────────────────────────────────────────────────────────────────┘
```

The GIS trifecta (**Velo**, **Carta**, **Locus**) provides complete geographic functionality:
- **Velo**: Routing (A* search, turn-by-turn navigation)
- **Carta**: Map tiles (vector MVT, raster PNG)
- **Locus**: Geocoding (forward search, autocomplete, reverse lookup)

See `docs/ARCHITECTURE.md` for detailed architecture documentation.
See `docs/TODO_FEATURES.md` for planned component specifications.

## Vendor Libraries

Each vendor library has its own CLAUDE.md with API documentation:

| Library | Location | Documentation | Purpose |
|---------|----------|---------------|---------|
| **miniz** | `vendor/miniz/` | [`vendor/miniz/CLAUDE.md`](vendor/miniz/CLAUDE.md) | zlib-compatible compression (DEFLATE, ZIP) |
| **mongoose** | `vendor/mongoose/` | [`vendor/mongoose/CLAUDE.md`](vendor/mongoose/CLAUDE.md) | Embedded HTTP/WebSocket server |
| **clay** | `vendor/clay/` | [`vendor/clay/CLAUDE.md`](vendor/clay/CLAUDE.md) | High-performance 2D UI layout |

## UI System (ClayShards + Immediate Mode)

The platform uses a hybrid UI architecture: **Clay** for declarative layout + **ClayShards** immediate mode components for interaction.

| Component | Location | Purpose |
|-----------|----------|---------|
| **clay-shards** | `clayshards/clay-shards/` | Immediate mode components (button, input, map) |
| **clay-shards-webgl** | `clayshards/clay-shards-webgl/` | WebGL renderer for browsers |
| **clay-shards-demo** | `clayshards/clay-shards-demo/` | Example map viewer application |
| **fonts** | `clayshards/fonts/` | MSDF font assets for UI rendering |

### Architecture

```
Application: if (cs_button(...).clicked) { ... }
     │
ClayShards: Immediate mode API, focus/click handling
     │
Clay: Declarative layout, render commands
     │
Renderer: WebGL (browser), SDL/raylib (native) [planned]
```

### Key Files

- `clayshards/clay-shards/include/cs_common.h` - Core API, allocator
- `clayshards/clay-shards/src/cs_common.c` - State, keyboard handling, TLS
- `clayshards/clay-shards/src/cs_map.c` - Map pan/zoom, overlays
- `clayshards/clay-shards/src/cs_map_projection.c` - Web Mercator utilities
- `clayshards/clay-shards/src/cs_map_simplify.c` - Douglas-Peucker (iterative)
- `clayshards/clay-shards-webgl/renderer.js` - WebGL renderer

See `clayshards/clay-shards/CLAUDE.md` for detailed API documentation.

### Features

- **Thread-local storage**: Each thread gets isolated UI state
- **Custom allocators**: Plug in arena allocators or debug allocators
- **Error tracking**: `cs_get_last_error()`, `cs_get_error_count()`
- **52 unit tests** covering edge cases and stress conditions

### Future Renderers

Planned native backends:
- `clay-shards-sdl/` - SDL2 for desktop/mobile
- `clay-shards-raylib/` - raylib for games
- `clay-shards-sokol/` - Sokol for minimal deps

## Critical Code Sections

### Ralph: LU Factorization (`ralph/src/lu.c`)
Most sensitive - bugs cause wrong solutions:
- `lu_factorize()` - Initial factorization
- `lu_solve()` - Solve Bx = b
- `lu_update()` - Basis change update

### Velo: A* Bidirectional (`velo/src/vl_route.c`)
Performance-critical routing:
- `vl_route_astar_bidir()` - Main routing function
- Consistent heuristic for bidirectional search

### Carta: MVT Encoding (`carta/src/ct_mvt.c`)
Geometry encoding:
- Delta + zigzag coordinate encoding
- MoveTo/LineTo/ClosePath commands

### FuelWise: LP Formulation (`fuelwise/src/fw_refuel.c`)
- Variables: x[i] (purchases), y[i] (cumulative fuel)
- Constraints: fuel balance, capacity, minimum levels

## Testing

```bash
make test
# Expected: ~279 tests pass across all modules
# - ralph: 73 tests
# - fuelwise: 33 tests
# - shared: 41 tests
# - velo: 47 tests
# - carta: 33 tests
# - locus: 52 tests
```

## Performance Targets

| Operation | Target |
|-----------|--------|
| LP solve (1000 vars) | < 100ms |
| Route (country-scale) | < 100ms |
| PNG tile (512x512) | < 100ms |
| Refuel optimization | < 100ms |
| PBF parse (300MB) | < 15s |
| Forward geocoding | < 20µs |
| Autocomplete | < 20µs |
| Reverse geocoding | < 30µs |

## Common Pitfalls

1. **Ralph**: RHS must be non-negative for constraints
2. **Velo**: Bidirectional search needs consistent heuristic
3. **Carta**: Coordinate order is (lon, lat) in MVT
4. **Locus**: All text queries must be UTF-8 encoded
5. **FuelWise**: Stations must be sorted by distance_from_start
6. **Memory**: Free all allocated structures (solutions, routes, contexts)

## C Memory Safety Guidelines

OTTO is written in C, which requires discipline to avoid memory vulnerabilities. Follow these patterns rigorously.

### Arena Allocation (Preferred)

Most OTTO modules use arena allocators - allocate in bulk, free in bulk:

```c
/* Good: Arena allocation */
Arena arena = arena_create(buffer, size);
Node *nodes = arena_alloc(&arena, n * sizeof(Node));
Edge *edges = arena_alloc(&arena, m * sizeof(Edge));
/* ... use nodes and edges ... */
arena_reset(&arena);  /* Free everything at once */
```

**Benefits:** No double-free, no dangling pointers, no fragmentation, cache-friendly.

**Used in:** Ralph solver, Velo routing, Carta tile generation.

### Ownership Rules

When arena allocation isn't suitable:

```c
/* Rule: _create() implies _destroy() */
Graph *g = graph_create();
/* ... */
graph_destroy(g);
g = NULL;  /* Prevent use-after-free */

/* Rule: Document ownership in function names */
Solution *solver_solve(Problem *p);    /* Returns owned pointer - caller frees */
void process_data(const Data *d);      /* Borrows pointer - caller retains ownership */
```

### Buffer Safety

```c
/* NEVER use unbounded string operations */
strcpy(dest, src);                    /* NO - buffer overflow */
sprintf(buf, "%s: %d", name, val);    /* NO - buffer overflow */

/* ALWAYS use bounded versions */
strncpy(dest, src, sizeof(dest) - 1);
dest[sizeof(dest) - 1] = '\0';

snprintf(buf, sizeof(buf), "%s: %d", name, val);  /* Truncates safely */

/* ALWAYS validate array indices */
if (index < 0 || index >= count) {
    return ERROR_OUT_OF_BOUNDS;
}
```

### Integer Overflow

```c
/* DANGEROUS: Multiplication can overflow */
void *p = malloc(count * element_size);  /* Integer overflow possible! */

/* SAFE: Check before multiply */
if (count > SIZE_MAX / element_size) {
    return NULL;  /* Would overflow */
}
void *p = malloc(count * element_size);

/* SAFE: calloc checks internally */
void *p = calloc(count, element_size);
```

### Initialization

```c
/* DANGEROUS: Uninitialized data */
Config cfg;
use_config(&cfg);  /* Garbage values! */

/* SAFE: Zero-initialize */
Config cfg = {0};

/* SAFE: Designated initializers */
Config cfg = {
    .timeout_ms = 5000,
    .retries = 3,
};
```

### Post-Free Hygiene

```c
/* Pattern: NULL immediately after free */
void resource_destroy(Resource **r) {
    if (r && *r) {
        free((*r)->data);
        free(*r);
        *r = NULL;  /* Prevent double-free and use-after-free */
    }
}
```

### Defensive Macros

```c
/* Helper macros used in OTTO */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)

#define CHECK_NULL(p) do { \
    if ((p) == NULL) return ERROR_NULL; \
} while(0)

#define CHECK_BOUNDS(i, n) do { \
    if ((i) < 0 || (size_t)(i) >= (n)) return ERROR_BOUNDS; \
} while(0)
```

### When Reviewing C Code

Check for these issues:

| Issue | What to Look For |
|-------|------------------|
| **Buffer overflow** | `strcpy`, `sprintf`, `gets`, unbounded loops |
| **Integer overflow** | `malloc(a * b)` without overflow check |
| **Use-after-free** | Pointer used after `free()` call |
| **Double-free** | `free()` called twice on same pointer |
| **Null deref** | Pointer used without NULL check |
| **Uninitialized** | Variables used before assignment |
| **Memory leak** | `malloc` without corresponding `free` |

### Build Flags

Use these flags in development:

```bash
# Warnings (CI should use -Werror)
CFLAGS += -Wall -Wextra -Wconversion -Wshadow -Wformat=2

# Debug builds: AddressSanitizer + UndefinedBehaviorSanitizer
CFLAGS += -fsanitize=address,undefined -g

# Test with Valgrind
valgrind --leak-check=full --track-origins=yes ./test_runner
```

### WASM Defense-in-Depth

Even buggy C code is partially contained in WASM:
- Linear memory is bounds-checked
- Cannot access host memory
- Stack overflow traps instead of corrupting memory
- No arbitrary code execution

This doesn't excuse bugs, but limits blast radius.

## Scripts

Utility scripts in `scripts/` directory:

```bash
# Download OSM data from Geofabrik
./scripts/download-osm.sh hungary    # Download Hungary
./scripts/download-osm.sh monaco     # Download Monaco (small, for testing)
./scripts/download-osm.sh list       # List available regions

# Performance benchmarks
./scripts/benchmark.sh               # Run all benchmarks
./scripts/benchmark.sh velo          # Routing benchmarks
./scripts/benchmark.sh carta         # Tile generation benchmarks

# CI/CD pipeline
./scripts/ci.sh                      # Full pipeline (build, test, lint)
./scripts/ci.sh quick                # Quick build + test
./scripts/ci.sh lint                 # Lint only
```

## Docker

Multi-service Docker Compose setup:

```bash
docker-compose up                       # Full platform (FuelWise + UI)
docker-compose --profile all-apis up    # All 3 API servers
docker-compose --profile dev up         # Development environment
```

Individual Dockerfiles:
- `Dockerfile` - Main FuelWise image
- `docker/Dockerfile.velo` - Standalone Velo route server
- `docker/Dockerfile.carta` - Standalone Carta tile server

## Planned Components Overview

The following components are documented in detail in `docs/TODO_FEATURES.md`:

### HoSE - Hours of Service Engine
- FMCSA 4-clock model (8h break, 11h drive, 14h shift, 70h cycle)
- EU EC/561 rules (4.5h drive, 9/10h daily, 56h weekly, 90h bi-weekly)
- Transit + loading algorithm with break scheduling
- Pattern-based acceleration for long-haul

### Tempo - Business Rules Engine
- Time window constraints (continuous, recurring, recurring without weekends)
- Intermediate tasks (ITSKs) placement
- Facility hours, blackout periods
- Max transit constraints

### Arbor - State-Space Search Engine
- Generic state-space search framework
- DFS with explicit stack, best-first, beam search
- Composable pruning heuristics
- Solution pool management

### Sigma - Fleet Plan Selection Engine
- Set covering/partitioning MIP formulation
- Integrates with Ralph solver
- Column generation for large-scale problems
- Vehicle/driver assignment constraints

### Pulse - Execution Tracker and PTA Engine
- Forward simulation of plan execution
- Predicted Time of Arrival (PTA) computation
- Automatic break insertion using HoSE
- Time window validation using Tempo

## Design Principles

1. **Zero Dependencies** - Core libraries use only standard C
2. **WASM-First** - All components compile to WebAssembly
3. **Layered** - solver → domain → api → ui
4. **Portable** - Linux, macOS, Windows, browsers
