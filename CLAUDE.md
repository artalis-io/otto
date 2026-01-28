# Claude Code Instructions for FuelWise Platform

## Project Overview

FuelWise is a truck fleet optimization platform written primarily in C with a TypeScript React frontend. It combines route planning, fuel cost optimization, and custom map rendering. All core libraries are zero-dependency and compile to WebAssembly.

## Quick Start

```bash
# Build all libraries
make all

# Run all tests (150+ across all modules)
make test

# Start servers
make run-api              # API on :8080
cd ui && npm run dev      # UI on :5173
```

## Component Summary

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| Ralph | `ralph/` | C | LP/MIP solver engine |
| Velo | `velo/` | C | OSM routing engine |
| Carta | `carta/` | C | Map tile generator (MVT/PNG) |
| FuelWise | `fuelwise/` | C | Refueling domain logic |
| Shared | `shared/` | C | Common geo utilities |
| Vendor | `vendor/miniz/` | C | Vendored zlib (miniz) |
| API | `api/` | C | REST API server |
| WASM | `wasm/` | C+JS | Browser builds |
| UI | `ui/` | TypeScript | React frontend |

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

### Working on API:
- `api/src/main.c` - HTTP handlers

### Working on UI:
- `ui/src/App.tsx` - Main component
- `ui/src/components/MapView.tsx` - Map integration

## Build Commands

```bash
# All libraries
make all            # ralph + fuelwise + shared + velo + carta

# Individual modules
make ralph          # LP/MIP solver
make fuelwise       # Refueling library
make shared         # Shared geo utilities
make velo           # Routing engine
make carta          # Tile generator
make api            # REST API server
make wasm           # WebAssembly (needs Emscripten)

# Testing
make test           # All tests (~160 tests)
make test-ralph     # 65 tests
make test-fuelwise  # 32 tests
make test-shared    # 23 tests
make test-velo      # 39 tests
make test-carta     # 33 tests

# Run
make run-api        # Start API server
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/filter` | POST | Filter stations to route |
| `/api/v1/solve` | POST | Solve refueling problem |
| `/api/v1/optimize` | POST | Full pipeline |

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  UI (React) / WASM (Browser) / API (mongoose)          │
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

## Design Principles

1. **Zero Dependencies** - Core libraries use only standard C
2. **WASM-First** - All components compile to WebAssembly
3. **Layered** - solver → domain → api → ui
4. **Portable** - Linux, macOS, Windows, browsers
