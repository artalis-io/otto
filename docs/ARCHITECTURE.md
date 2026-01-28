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
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                │   │
│  │  │   FuelWise   │  │     Velo     │  │    Carta     │                │   │
│  │  │   Refueling  │  │   Routing    │  │  Map Tiles   │                │   │
│  │  │ Optimization │  │   Engine     │  │  Generator   │                │   │
│  │  │              │  │              │  │              │                │   │
│  │  │ libfuelwise.a│  │  libvelo.a   │  │ libcarta.a   │                │   │
│  │  └──────┬───────┘  └──────────────┘  └──────────────┘                │   │
│  └─────────┼────────────────────────────────────────────────────────────┘   │
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
fuelwise/
├── ralph/              # LP/MIP Solver (libralph.a)
│   ├── include/        #   Public headers
│   ├── src/            #   Simplex, LU factorization, Branch & Bound
│   └── tests/          #   43 tests
│
├── fuelwise/           # Refueling Library (libfuelwise.a)
│   ├── include/        #   Public headers
│   ├── src/            #   LP formulation, route filtering
│   └── tests/          #   29 tests
│
├── velo/               # Routing Engine (libvelo.a)
│   ├── include/        #   Public headers
│   ├── src/            #   OSM PBF parsing, Dijkstra, A*, landmarks
│   └── tests/          #   30+ tests
│
├── carta/              # Tile Generator (libcarta.a)
│   ├── include/        #   Public headers
│   ├── src/            #   MVT encoding, PNG rendering, Web Mercator
│   ├── examples/       #   Usage examples
│   └── tests/          #   33 tests
│
├── shared/             # Shared utilities (libshared.a)
│   ├── include/        #   Common headers
│   ├── src/            #   Geo utilities, protobuf helpers
│   └── tests/          #   Shared code tests
│
├── vendor/             # Third-party code (header-only or vendored)
│   └── miniz/          #   Public domain zlib implementation
│
├── api/                # REST API Server
│   ├── src/            #   HTTP handlers
│   └── mongoose/       #   Embedded HTTP server
│
├── wasm/               # WebAssembly Builds
│   └── src/            #   WASM bindings
│
├── ui/                 # React Application
│   └── src/            #   TypeScript frontend
│
├── docs/               # Documentation
│   ├── ARCHITECTURE.md #   This file
│   ├── API.md          #   REST API reference
│   └── *.md            #   Technical docs
│
└── tests/              # Integration tests
    └── integration/    #   End-to-end tests
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
    ┌─────────────────────┼─────────────────────┐
    │                     │                     │
    ▼                     ▼                     ▼
┌─────────┐         ┌──────────┐         ┌──────────┐
│FuelWise │         │   Velo   │         │  Carta   │
│Refueling│         │ Routing  │         │  Tiles   │
└────┬────┘         └────┬─────┘         └────┬─────┘
     │                   │                    │
     │                   └────────┬───────────┘
     │                            │
     ▼                            ▼
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
- **clay**: High-performance 2D UI layout library ([vendor/clay/CLAUDE.md](../vendor/clay/CLAUDE.md))

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

## Build System

### Top-Level Targets
```makefile
make all          # Build all libraries (ralph, fuelwise, velo, carta, shared)
make test         # Run all tests (~135 tests)
make clean        # Clean all build artifacts
make api          # Build REST API server
make wasm         # Build WebAssembly modules
```

### Module Targets
```makefile
make ralph        # Build libralph.a
make fuelwise     # Build libfuelwise.a (depends on ralph)
make velo         # Build libvelo.a (depends on shared, vendor)
make carta        # Build libcarta.a (depends on shared, vendor)
make shared       # Build libshared.a
```

### Testing
```makefile
make test-ralph     # 43 tests
make test-fuelwise  # 29 tests
make test-velo      # 30+ tests
make test-carta     # 33 tests
make test-shared    # Shared code tests
```

## Design Principles

1. **Zero Dependencies**: Core libraries use only standard C (C11)
2. **WASM-First**: All components compile to WebAssembly
3. **Layered Architecture**: Clear separation: solver → domain → API → UI
4. **Portable**: Runs on Linux, macOS, Windows, browsers
5. **Single Responsibility**: Each module has a focused purpose

## Performance Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| LP solve (1000 vars) | <100ms | Ralph simplex |
| Route (country-scale) | <100ms | Velo A* bidirectional |
| PNG tile (512x512) | ~75ms | Carta rasterizer |
| Refuel optimization | <100ms | FuelWise LP |
| PBF parse (Hungary) | ~10s | 300MB, 35M nodes |

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

## Medium Priority

- [ ] **Standardize test structure**
  - Currently inconsistent: inline tests vs separate files
  - Adopt single pattern across all modules
  - Consider unified test runner

- [ ] **Add integration tests at root**
  - Create `tests/integration/` directory
  - End-to-end tests (route + refuel + tiles)
  - API endpoint tests with curl/shell

## Low Priority

- [ ] **Add scripts/ directory**
  - `download-osm.sh` - Fetch OSM data from Geofabrik
  - `gen-types.sh` - Generate TypeScript types from C headers
  - `benchmark.sh` - Run all benchmarks
  - `ci.sh` - CI/CD pipeline script

- [ ] **Improve documentation**
  - Add API.md for each module (not just root)
  - Add CONTRIBUTING.md
  - Add CHANGELOG.md
