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
cd fuelwise/ui && npm run dev  # UI on :5173
```

## Component Summary

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| Ralph | `ralph/` | C | LP/MIP solver engine |
| Velo | `velo/` | C | OSM routing engine |
| Carta | `carta/` | C | Map tile generator (MVT/PNG) |
| FuelWise | `fuelwise/` | C | Refueling domain logic |
| Shared | `shared/` | C | Common geo utilities |
| Vendor | `vendor/` | C | Third-party libs (see below) |
| FuelWise API | `fuelwise/api/` | C | Refueling REST API |
| Route Server | `velo/api/` | C | Routing REST API |
| Tile Server | `carta/api/` | C | Tile server REST API |
| FuelWise WASM | `fuelwise/wasm/` | C+JS | Browser builds |
| UI | `fuelwise/ui/` | TypeScript | React frontend |

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

### Working on UI:
- `fuelwise/ui/src/App.tsx` - Main component
- `fuelwise/ui/src/components/MapView.tsx` - Map integration

## Build Commands

```bash
# Libraries
make all            # Build all with tests (default)
make lib            # Build libraries only (no tests)
make ralph          # LP/MIP solver
make fuelwise       # Refueling library
make shared         # Shared geo utilities
make velo           # Routing engine
make carta          # Tile generator

# API Servers
make api            # FuelWise REST API (fuelwise/api)
make route-server   # Velo route server (velo/api)
make tile-server    # Carta tile server (carta/api)

# WebAssembly (requires Emscripten)
make wasm           # Build all WASM modules
make wasm-fuelwise  # FuelWise WASM only
make wasm-velo      # Velo WASM only
make wasm-carta     # Carta WASM only
make wasm-types     # Generate TypeScript declarations
make wasm-test      # Test WASM builds

# Testing
make test           # All tests (~190 tests)
make test-ralph     # 65 tests
make test-fuelwise  # 32 tests
make test-shared    # 23 tests
make test-velo      # 39 tests
make test-carta     # 33 tests

# Run servers
make run-api        # Start FuelWise API server
make run-routes     # Show route server usage
make run-tiles      # Show tile server usage
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

## Design Principles

1. **Zero Dependencies** - Core libraries use only standard C
2. **WASM-First** - All components compile to WebAssembly
3. **Layered** - solver → domain → api → ui
4. **Portable** - Linux, macOS, Windows, browsers
