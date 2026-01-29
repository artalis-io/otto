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
| FuelWise | `fuelwise/` | C | Refueling domain logic |
| Shared | `shared/` | C | Common geo utilities |
| Vendor | `vendor/` | C | Third-party libs (see below) |

### Planned Components

| Component | Location | Purpose |
|-----------|----------|---------|
| Nexus | `nexus/` | **N**ormalized **Ex**ternal **U**nified **S**napshots - TMS/ELD/LoadBoard integration gateway |
| HoSE | `hose/` | **H**ours **o**f **S**ervice **E**ngine - FMCSA/EC561 compliance |
| Tempo | `tempo/` | **T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator |
| Arbor | `arbor/` | **A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime |
| Sigma | `sigma/` | **S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation |
| Pulse | `pulse/` | **P**lan **U**tilization and **L**ive **S**tate **E**stimator |

See `docs/TODO_FEATURES.md` for detailed specifications of planned components.
See `docs/NEXUS.md` for the data ingress architecture (TMS/ELD/LoadBoard integration).

### Applications

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| FuelWise API | `fuelwise/api/` | C | Refueling REST API |
| Route Server | `velo/api/` | C | Routing REST API |
| Tile Server | `carta/api/` | C | Tile server REST API |
| FuelWise WASM | `fuelwise/wasm/` | C+JS | Browser builds |
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

### Working on refueling:
- `fuelwise/include/fuelwise.h` - Library API
- `fuelwise/src/fw_refuel.c` - LP/MILP formulation
- `fuelwise/src/fw_route.c` - Station filtering

### Working on geo utilities (shared):
- `shared/include/shared.h` - Shared API
- `shared/include/sh_geo.h` - Coordinate types and functions
- `shared/src/sh_geo.c` - Haversine, Web Mercator

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

# API Servers
make fuelwise-api     # FuelWise REST API (fuelwise/api)
make velo-api         # Velo route server (velo/api)
make carta-api        # Carta tile server (carta/api)

# WebAssembly (requires Emscripten)
make wasm             # Build all WASM modules
make wasm-fuelwise    # FuelWise WASM only
make wasm-velo        # Velo WASM only
make wasm-carta       # Carta WASM only
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
make test-api         # All API tests (requires OSM data)
make test-fuelwise-api# FuelWise API tests
make test-velo-api    # Velo API tests
make test-carta-api   # Carta API tests

# Scripts
make benchmark        # Run performance benchmarks
make ci               # Run CI pipeline

# Run servers
make run-fuelwise-api # Start FuelWise API on :8080
make run-velo-api     # Show Velo route server usage
make run-carta-api    # Show Carta tile server usage
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

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  UI (React) / WASM (Browser) / API (mongoose)          │
├─────────────────────────────────────────────────────────┤
│  Fleet Optimization [PLANNED]                           │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌────────┐ ┌───────┐ │
│  │  HoSE  │ │ Tempo  │ │ Arbor  │ │ Sigma  │ │ Pulse │ │
│  │  HoS   │ │ Time   │ │ Search │ │ Fleet  │ │ PTA   │ │
│  └────────┘ └────────┘ └────────┘ └────────┘ └───────┘ │
├─────────────────────────────────────────────────────────┤
│  Domain Libraries                                       │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐       │
│  │  FuelWise   │ │    Velo     │ │   Carta     │       │
│  │  Refueling  │ │   Routing   │ │   Tiles     │       │
│  └──────┬──────┘ └──────┬──────┘ └──────┬──────┘       │
│         │               │               │               │
│  ┌──────┴───────┐       └───────┬───────┘               │
│  │    Ralph     │        ┌──────┴──────┐                │
│  │  LP/MIP      │        │   shared    │                │
│  │  Solver      │        │  (geo,proj) │                │
│  └──────────────┘        └──────┬──────┘                │
│                                 │                       │
│                          ┌──────┴──────┐                │
│                          │   vendor    │                │
│                          │  (miniz)    │                │
│                          └─────────────┘                │
└─────────────────────────────────────────────────────────┘
```

See `docs/ARCHITECTURE.md` for detailed architecture documentation.
See `docs/TODO_FEATURES.md` for planned component specifications.

## Vendor Libraries

Each vendor library has its own CLAUDE.md with API documentation:

| Library | Location | Documentation | Purpose |
|---------|----------|---------------|---------|
| **miniz** | `vendor/miniz/` | [`vendor/miniz/CLAUDE.md`](vendor/miniz/CLAUDE.md) | zlib-compatible compression (DEFLATE, ZIP) |
| **mongoose** | `vendor/mongoose/` | [`vendor/mongoose/CLAUDE.md`](vendor/mongoose/CLAUDE.md) | Embedded HTTP/WebSocket server |
| **clay** | `vendor/clay/` | [`vendor/clay/CLAUDE.md`](vendor/clay/CLAUDE.md) | High-performance 2D UI layout |

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
# Expected: ~190 tests pass across all modules
# - ralph: 65 tests
# - fuelwise: 32 tests
# - shared: 23 tests
# - velo: 39 tests
# - carta: 33 tests
```

## Performance Targets

| Operation | Target |
|-----------|--------|
| LP solve (1000 vars) | < 100ms |
| Route (country-scale) | < 100ms |
| PNG tile (512x512) | < 100ms |
| Refuel optimization | < 100ms |
| PBF parse (300MB) | < 15s |

## Common Pitfalls

1. **Ralph**: RHS must be non-negative for constraints
2. **Velo**: Bidirectional search needs consistent heuristic
3. **Carta**: Coordinate order is (lon, lat) in MVT
4. **FuelWise**: Stations must be sorted by distance_from_start
5. **Memory**: Free all allocated structures (solutions, routes, contexts)

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
