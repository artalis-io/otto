# FuelWise Platform Architecture

## Overview

FuelWise is a truck refueling optimization platform built on a layered architecture of zero-dependency C libraries. The platform is designed for both native deployment and WebAssembly (WASM) for browser-based applications.

## System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Applications                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐                   │
│  │  React UI   │     │    WASM     │     │  REST API   │                   │
│  │  (Leaflet)  │     │  (Browser)  │     │ (mongoose)  │                   │
│  └──────┬──────┘     └──────┬──────┘     └──────┬──────┘                   │
│         └───────────────────┼───────────────────┘                           │
├─────────────────────────────┼───────────────────────────────────────────────┤
│                             ▼                                                │
│                      Domain Libraries                                        │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────┐ ┌────────────┐        │   │
│  │  │  FuelWise  │ │    Velo    │ │   Carta    │ │   Locus    │        │   │
│  │  │ Refueling  │ │  Routing   │ │ Map Tiles  │ │ Geocoding  │        │   │
│  │  │Optimization│ │  Engine    │ │ Generator  │ │  Engine    │        │   │
│  │  │            │ │            │ │            │ │            │        │   │
│  │  │libfuelwise │ │ libvelo.a  │ │libcarta.a  │ │liblocus.a  │        │   │
│  │  └─────┬──────┘ └────────────┘ └────────────┘ └────────────┘        │   │
│  └────────┼─────────────────────────────────────────────────────────────┘   │
│            │                                                                 │
├────────────┼─────────────────────────────────────────────────────────────────┤
│            ▼                                                                 │
│                         Core Solver                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        Ralph Solver                                   │   │
│  │  ┌──────────┐  ┌───────────┐  ┌───────────────┐  ┌───────────┐      │   │
│  │  │ Simplex  │  │    LU     │  │ Branch&Bound  │  │  Gomory   │      │   │
│  │  │ Revised  │  │ Factorize │  │     MIP       │  │   Cuts    │      │   │
│  │  └──────────┘  └───────────┘  └───────────────┘  └───────────┘      │   │
│  │                                                                       │   │
│  │                         libralph.a                                    │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│                       Shared Infrastructure                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐     │   │
│  │  │   shared   │  │   vendor   │  │  Haversine │  │  Protobuf  │     │   │
│  │  │    geo     │  │   miniz    │  │   (geo)    │  │  (decode)  │     │   │
│  │  └────────────┘  └────────────┘  └────────────┘  └────────────┘     │   │
│  │                                                                       │   │
│  │                  libshared.a  +  vendor/*.o                           │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Directory Structure

```
fuelwise-platform/
├── ralph/              # LP/MIP Solver (libralph.a)
│   ├── include/        #   Public headers
│   ├── src/            #   Simplex, LU factorization, Branch & Bound
│   └── tests/          #   65 tests
│
├── fuelwise/           # Refueling Library (libfuelwise.a)
│   ├── include/        #   Public headers
│   ├── src/            #   LP formulation, route filtering
│   ├── tests/          #   32 tests
│   ├── api/            #   FuelWise REST API server
│   ├── wasm/           #   WebAssembly build
│   └── ui/             #   React frontend application
│
├── velo/               # Routing Engine (libvelo.a)
│   ├── include/        #   Public headers
│   ├── src/            #   OSM PBF parsing, Dijkstra, A*, landmarks
│   ├── tests/          #   39 tests
│   ├── api/            #   Velo Route Server REST API
│   └── wasm/           #   WebAssembly build
│
├── carta/              # Tile Generator (libcarta.a)
│   ├── include/        #   Public headers
│   ├── src/            #   MVT encoding, PNG rendering, Web Mercator
│   ├── tests/          #   33 tests
│   ├── api/            #   Carta Tile Server REST API
│   ├── ui/             #   Tile Viewer React Application
│   └── wasm/           #   WebAssembly build
│
├── locus/              # Geocoding Engine (liblocus.a)
│   ├── include/        #   Public headers (locus.h, lc_*.h)
│   ├── src/            #   Trie, n-gram, spatial index, PBF parsing
│   ├── tests/          #   52 tests
│   ├── api/            #   Locus Geocoding Server REST API
│   ├── tools/          #   CLI search utilities
│   └── wasm/           #   WebAssembly build
│
├── shared/             # Shared utilities (libshared.a)
│   ├── include/        #   Common headers
│   ├── src/            #   Geo utilities, protobuf helpers
│   └── tests/          #   23 tests
│
├── forge/              # Async Job Queue [PLANNED]
│   ├── include/        #   Public headers
│   ├── src/            #   Broker, dispatcher, SQLite persistence
│   ├── api/            #   REST + WebSocket server (:8084)
│   └── consumers/      #   Built-in job consumers
│
├── vendor/             # Third-party code (vendored)
│   ├── miniz/          #   Public domain zlib implementation
│   ├── mongoose/       #   Embedded HTTP server
│   ├── sqlite/         #   Embedded SQL database [PLANNED]
│   └── clay/           #   UI layout library (future)
│
├── scripts/            # Utility scripts
│   ├── download-osm.sh #   Download OSM PBF from Geofabrik
│   ├── benchmark.sh    #   Performance benchmarks
│   └── ci.sh           #   CI/CD pipeline
│
├── docs/               # Documentation
│   ├── ARCHITECTURE.md #   This file
│   ├── KNOWN_ISSUES.md #   Known issues and limitations
│   └── *.md            #   Technical docs
│
├── docker/             # Docker configuration
│   ├── nginx.conf      #   Nginx config for UI proxy
│   └── entrypoint.sh   #   Container entrypoint
│
├── Makefile            # Top-level build orchestration
├── Dockerfile          # Multi-stage Docker build
├── docker-compose.yml  # Docker Compose config
├── CLAUDE.md           # Claude Code instructions
├── AGENTS.md           # AI agents guide
└── README.md           # Project documentation
```

## Module Dependency Graph

```
                    ┌─────────────┐
                    │   React UI  │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
              ▼            ▼            ▼
        ┌──────────┐ ┌──────────┐ ┌──────────┐
        │   WASM   │ │   API    │ │ Standalone│
        │ (browser)│ │ (server) │ │   CLI    │
        └────┬─────┘ └────┬─────┘ └────┬─────┘
             │            │            │
             └────────────┼────────────┘
                          │
    ┌──────────────────┼──────────────────┬──────────────────┐
    │                  │                  │                  │
    ▼                  ▼                  ▼                  ▼
┌─────────┐      ┌──────────┐      ┌──────────┐      ┌──────────┐
│FuelWise │      │   Velo   │      │  Carta   │      │  Locus   │
│Refueling│      │ Routing  │      │  Tiles   │      │ Geocoding│
└────┬────┘      └────┬─────┘      └────┬─────┘      └────┬─────┘
     │                │                 │                 │
     │                └─────────┬───────┴─────────────────┘
     │                          │
     ▼                          ▼
┌─────────┐              ┌──────────────┐
│  Ralph  │              │    shared    │
│ Solver  │              │ (geo, proto) │
└─────────┘              └──────┬───────┘
                                │
                                ▼
                         ┌──────────┐
                         │  vendor  │
                         │  miniz   │
                         └──────────┘
```

## Component Details

### Ralph - LP/MIP Solver
**Location:** `ralph/`
**Library:** `libralph.a`
**Dependencies:** None (zero-dependency)

Core optimization engine implementing:
- Revised Simplex Method with LU factorization
- Branch and Bound for mixed-integer problems
- Gomory cutting planes
- Sparse matrix operations (CSC format)

See [RALPH_ARCHITECTURE.md](RALPH_ARCHITECTURE.md) for detailed solver internals.

### FuelWise - Refueling Optimization
**Location:** `fuelwise/`
**Library:** `libfuelwise.a`
**Dependencies:** `libralph.a`

Domain-specific library for truck refueling:
- LP/MILP formulation for minimum-cost fueling
- Route corridor filtering
- Piecewise fuel consumption support

### Velo - Routing Engine
**Location:** `velo/`
**Library:** `libvelo.a`
**Dependencies:** `libshared.a`, `vendor/miniz`

OSM-based routing engine:
- OSM PBF parsing with protobuf decoding
- Dijkstra, A*, bidirectional A*
- ALT (A* Landmarks Triangle inequality)
- Vehicle profile-based edge filtering
- CSR graph representation

### Carta - Tile Generator
**Location:** `carta/`
**Library:** `libcarta.a`
**Dependencies:** `libshared.a`, `vendor/miniz`

Map tile generation:
- Vector tiles (MVT/Mapbox Vector Tile)
- Raster tiles (PNG)
- Web Mercator projection
- Software rasterization with anti-aliasing

### Locus - Geocoding Engine
**Location:** `locus/`
**Library:** `liblocus.a`
**Dependencies:** `libshared.a`, `vendor/miniz`

**L**ocation **O**riented **C**oordinate **U**nification **S**ystem - geocoding for OSM data:
- Forward geocoding (text search → coordinates)
- Autocomplete (prefix-based type-ahead suggestions)
- Reverse geocoding (coordinates → nearest address/place)
- Fuzzy matching using trigram similarity
- Binary index format (.lcix) for fast loading

Data structures:
- **Prefix Trie**: O(m) lookup for autocomplete (m = query length)
- **Trigram Index**: Jaccard similarity for fuzzy matching
- **Spatial Grid**: Cell-based geographic index for reverse geocoding

The GIS trifecta (**Velo**, **Carta**, **Locus**) provides complete geographic functionality:
- **Velo**: Routing (A* search, turn-by-turn navigation)
- **Carta**: Map tiles (vector MVT, raster PNG)
- **Locus**: Geocoding (forward search, autocomplete, reverse lookup)

### Shared - Common Utilities
**Location:** `shared/`
**Library:** `libshared.a`
**Dependencies:** None

Common code used by velo and carta:
- Haversine distance calculation
- Coordinate projections
- Protobuf varint encoding/decoding

### Vendor - Third-party Code
**Location:** `vendor/`
**Dependencies:** None

Vendored libraries (each has its own `CLAUDE.md` with API documentation):
- **miniz**: Public domain zlib implementation for DEFLATE compression ([vendor/miniz/CLAUDE.md](../vendor/miniz/CLAUDE.md))
- **mongoose**: Embedded HTTP server library ([vendor/mongoose/CLAUDE.md](../vendor/mongoose/CLAUDE.md))
- **sqlite**: Embedded SQL database (planned, for Forge job persistence)
- **clay**: High-performance 2D UI layout library ([vendor/clay/CLAUDE.md](../vendor/clay/CLAUDE.md))

### Forge - Async Job Queue [PLANNED]
**Location:** `forge/`
**Dependencies:** `vendor/sqlite`, `vendor/mongoose`

Generic job broker for long-running async tasks. See [FORGE.md](FORGE.md) for full specification.

- Job submission with polling and WebSocket streaming
- SQLite persistence for durability
- Spawns consumer processes for each job type
- Language-agnostic consumer protocol (stdin/stdout/stderr)

Consumers are standalone executables registered by job type:
- `fg-ralph`: LP/MIP solving
- `fg-fuelwise`: Refueling optimization
- `fg-velo`: Batch routing
- `fg-locus`: Batch geocoding
- `fg-carta`: Tile generation

### Quota - Rate Quoting Engine [PLANNED]
**Location:** `quota/`
**Dependencies:** `libralph.a`, `libvelo.a`

**Q**uote **U**nderwriting and **T**ariff **O**ptimization **A**lgorithm. Spot and contract pricing engine.

- Cost modeling: fuel, driver, tolls, deadhead, maintenance
- Margin optimization based on lane, customer, market conditions
- Rate generation for spot quotes and contract RFPs
- Historical rate analysis and trend detection

### Atlas - Network Design Engine [PLANNED]
**Location:** `atlas/`
**Dependencies:** `libralph.a`, `libvelo.a`

**A**llocation and **T**actical **L**ane **A**nalysis **S**ystem. Network design and lane balancing.

- Headhaul/backhaul ratio optimization
- Deadhead accumulation prevention
- Capacity planning across lanes
- Network smoothing for new tendered loads
- Does not assign specific trucks - works at lane/region level

## Data Flow

### Refueling Optimization Flow
```
User Request → API/WASM → FuelWise
                              │
                    ┌─────────┴─────────┐
                    │                   │
                    ▼                   ▼
             Filter Stations      Build LP Model
             (fw_route.c)         (fw_refuel.c)
                    │                   │
                    └─────────┬─────────┘
                              │
                              ▼
                    Ralph Simplex Solver
                        (simplex.c)
                              │
                              ▼
                     Extract Solution
                              │
                              ▼
                    Return Fuel Stops
```

### Tile Generation Flow
```
Tile Request → API/WASM → Carta
                              │
                              ▼
                    Load PBF Context
                       (ct_pbf.c)
                              │
                    ┌─────────┴─────────┐
                    │                   │
                    ▼                   ▼
              Vector Tile          Raster Tile
              (ct_mvt.c)          (ct_render.c)
                    │                   │
                    ▼                   ▼
              MVT Protobuf         PNG Encode
               Encoding            (ct_png.c)
                    │                   │
                    └─────────┬─────────┘
                              │
                              ▼
                      Return Tile Bytes
```

### Routing Flow
```
Route Request → API/WASM → Velo
                              │
                              ▼
                    Load Graph from PBF
                       (vl_pbf.c)
                              │
                              ▼
                    Build CSR Graph
                      (vl_graph.c)
                              │
                              ▼
                    A* / Dijkstra Search
                      (vl_route.c)
                              │
                              ▼
                    Return Route Path
```

### Geocoding Flow
```
Search Request → API/WASM → Locus
                              │
                              ▼
                    Load Index from PBF/.lcix
                       (lc_pbf.c / lc_serialize.c)
                              │
                              ▼
                    Build Search Index
                      (lc_index.c)
                              │
                    ┌─────────┴─────────┐
                    │                   │
                    ▼                   ▼
             Forward Search      Reverse Geocode
             (lc_trie.c)        (lc_spatial.c)
                    │                   │
                    ▼                   │
             Fuzzy Fallback             │
             (lc_ngram.c)               │
                    │                   │
                    └─────────┬─────────┘
                              │
                              ▼
                    Return Results
```

## Build System

### Top-Level Targets
```makefile
make all              # Build all libraries (ralph, fuelwise, velo, carta, shared)
make lib              # Build libraries only (no tests)
make test             # Run all tests (~192 tests)
make clean            # Clean all build artifacts
```

### API Server Targets
```makefile
make fuelwise-api     # Build FuelWise REST API (fuelwise/api)
make velo-api         # Build Velo route server (velo/api)
make carta-api        # Build Carta tile server (carta/api)
make locus-api        # Build Locus geocoding server (locus/api)
make run-fuelwise-api # Run FuelWise API on :8080
make run-velo-api     # Show Velo route server usage
make run-carta-api    # Show Carta tile server usage
make run-locus-api    # Show Locus geocoding server usage
```

### WebAssembly Targets
```makefile
make wasm             # Build all WASM modules
make wasm-fuelwise    # Build FuelWise WASM
make wasm-velo        # Build Velo WASM
make wasm-carta       # Build Carta WASM
make wasm-locus       # Build Locus WASM
make wasm-types       # Generate TypeScript declarations
make wasm-test        # Test WASM builds
```

### Module Targets
```makefile
make ralph        # Build libralph.a
make fuelwise     # Build libfuelwise.a (depends on ralph)
make velo         # Build libvelo.a (depends on shared, vendor)
make carta        # Build libcarta.a (depends on shared, vendor)
make locus        # Build liblocus.a (depends on shared, vendor)
make shared       # Build libshared.a
```

### Testing
```makefile
make test-ralph       # 65 tests
make test-fuelwise    # 32 tests
make test-velo        # 39 tests
make test-carta       # 33 tests
make test-locus       # 52 tests
make test-shared      # 23 tests
make test-api         # All API endpoint tests (requires OSM data)
make test-fuelwise-api# FuelWise API tests
make test-velo-api    # Velo API tests
make test-carta-api   # Carta API tests
make test-locus-api   # Locus API tests
```

## Design Principles

1. **Zero Dependencies**: Core libraries use only standard C (C11)
2. **WASM-First**: All components compile to WebAssembly
3. **Layered Architecture**: Clear separation: solver → domain → API → UI
4. **Portable**: Runs on Linux, macOS, Windows, browsers
5. **Single Responsibility**: Each module has a focused purpose
6. **EV & AV Ready**: Electric and autonomous vehicles are zero-day design principles:
   - EV: Range constraints, charging station routing, battery state-of-charge modeling
   - AV: Different cost structures, modified/eliminated HoS constraints, mixed fleet optimization
   - Vehicle type is a first-class parameter in HoSE, Velo, and FuelWise

## Performance Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| LP solve (1000 vars) | <100ms | Ralph simplex |
| Route (country-scale) | <100ms | Velo A* bidirectional |
| PNG tile (512x512) | ~75ms | Carta rasterizer |
| Refuel optimization | <100ms | FuelWise LP |
| PBF parse (Hungary) | ~10s | 300MB, 35M nodes |
| Forward geocoding | <20µs | Locus trie + trigram |
| Autocomplete | <20µs | Locus prefix trie |
| Reverse geocoding | <30µs | Locus spatial grid |

## WASM Considerations

- No `mmap` - use `malloc` + `fread`
- Single-threaded execution
- Memory limits for large PBF files
- Minimal API surface exported
- No file system access (data passed as buffers)

---

# TODO: Structural Improvements

The following improvements are planned for the project structure:

## High Priority

- [x] **Consolidate vendor/ directories** (COMPLETED)
  - Moved miniz from `velo/vendor/` and `carta/vendor/` to root `vendor/miniz/`
  - Updated Makefiles to reference `../vendor/miniz`
  - Single source of truth for vendored code

- [x] **Create shared/ library** (COMPLETED)
  - Created `shared/` with geo utilities (haversine, Web Mercator, coordinates)
  - `libshared.a` with 23 tests
  - Provides common types: `SHCoord`, `SHBBox`, coordinate conversions

- [x] **Reorganize API/WASM/UI directories** (COMPLETED)
  - Moved `api/` → `fuelwise/api/`
  - Moved `wasm/` → `fuelwise/wasm/`
  - Moved `ui/` → `fuelwise/ui/`
  - Created `velo/api/` for route server
  - Created `carta/api/` for tile server
  - Created `velo/wasm/` and `carta/wasm/` for WebAssembly builds

- [x] **Standardize Makefile targets** (COMPLETED)
  - Consistent targets across all modules: `all`, `lib`, `test`, `clean`, `help`
  - API targets: `fuelwise-api`, `velo-api`, `carta-api`
  - WASM targets: `wasm-fuelwise`, `wasm-velo`, `wasm-carta`
  - Run targets: `run-fuelwise-api`, `run-velo-api`, `run-carta-api`

## Medium Priority

- [x] **Add API endpoint tests** (COMPLETED)
  - `test-fuelwise-api` - FuelWise API curl tests
  - `test-velo-api` - Velo route server tests
  - `test-carta-api` - Carta tile server tests
  - `test-api` - Run all API tests

- [ ] **Standardize test structure**
  - Currently inconsistent: inline tests vs separate files
  - Adopt single pattern across all modules
  - Consider unified test runner

- [ ] **Add integration tests at root**
  - Create `tests/integration/` directory
  - End-to-end tests (route + refuel + tiles)
  - Cross-module integration tests

## Low Priority

- [x] **Add scripts/ directory** (COMPLETED)
  - `download-osm.sh` - Fetch OSM data from Geofabrik
  - `benchmark.sh` - Run all benchmarks
  - `ci.sh` - CI/CD pipeline script

- [x] **Add carta-ui** (COMPLETED)
  - Moved `carta/api/ui/` to `carta/ui/`
  - Added `carta-ui` and `carta-ui-dev` Makefile targets

- [x] **Docker multi-service** (COMPLETED)
  - `docker/Dockerfile.velo` - Velo route server
  - `docker/Dockerfile.carta` - Carta tile server
  - Updated `docker-compose.yml` with all 3 APIs

- [ ] **Improve documentation**
  - Add CONTRIBUTING.md
  - Add CHANGELOG.md

- [ ] **Generate TypeScript types**
  - `gen-types.sh` - Generate TypeScript types from C headers
