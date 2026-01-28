# FuelWise Platform

**Intelligent Truck Refueling & Route Optimization**

A complete platform for truck fleet optimization, combining route planning, fuel cost minimization, and custom map rendering. Built entirely in C with zero external dependencies, designed for native and WebAssembly deployment.

## Platform Components

| Component | Description | Status |
|-----------|-------------|--------|
| [**Ralph**](ralph/) | Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound) | Production |
| [**Velo**](velo/) | OSM routing engine (Dijkstra, A*, bidirectional, landmarks) | Production |
| [**Carta**](carta/) | Map tile generator (MVT vector tiles, PNG raster) | Production |
| [**FuelWise**](fuelwise/) | Refueling optimization library | Production |
| [**API**](api/) | REST API server | Production |
| [**WASM**](wasm/) | WebAssembly builds for browser deployment | Production |
| [**UI**](ui/) | React application with map interface | Production |

## Quick Start

### Using Docker

```bash
# Full platform (UI + API)
docker build -t fuelwise .
docker run -p 80:80 -p 8080:8080 fuelwise

# API only
docker build --target api-only -t fuelwise-api .
docker run -p 8080:8080 fuelwise-api
```

### Building from Source

```bash
# Build all C libraries
make all

# Run all tests (150+ tests across all modules)
make test

# Start API server
make run-api

# Build and run UI
cd ui && npm install && npm run dev
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         FuelWise Platform                               │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐                   │
│  │  React UI   │   │    WASM     │   │  REST API   │   Applications    │
│  │  (Leaflet)  │   │  (Browser)  │   │ (mongoose)  │                   │
│  └──────┬──────┘   └──────┬──────┘   └──────┬──────┘                   │
│         └─────────────────┼─────────────────┘                          │
│                           │                                             │
│  ┌────────────────────────┴────────────────────────────────────────┐   │
│  │                         Domain Libraries                         │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐           │   │
│  │  │   FuelWise   │  │     Velo     │  │    Carta     │           │   │
│  │  │   Refueling  │  │   Routing    │  │  Map Tiles   │           │   │
│  │  │ Optimization │  │   Engine     │  │  Generator   │           │   │
│  │  └──────┬───────┘  └──────────────┘  └──────────────┘           │   │
│  └─────────┼───────────────────────────────────────────────────────┘   │
│            │                                                            │
│  ┌─────────┴───────────────────────────────────────────────────────┐   │
│  │                        Ralph Solver                              │   │
│  │  ┌──────────┐  ┌───────────┐  ┌───────────────┐  ┌───────────┐  │   │
│  │  │ Simplex  │  │    LU     │  │ Branch&Bound  │  │  Gomory   │  │   │
│  │  │ Revised  │  │ Factorize │  │     MIP       │  │   Cuts    │  │   │
│  │  └──────────┘  └───────────┘  └───────────────┘  └───────────┘  │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │                     Shared Infrastructure                         │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐         │   │
│  │  │  miniz   │  │ Protobuf │  │ mongoose │  │Haversine │         │   │
│  │  │  (zlib)  │  │ (decode) │  │  (http)  │  │  (geo)   │         │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘         │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

## Project Structure

```
fuelwise/
├── ralph/          # LP/MIP Solver (libralph.a)
│   └── src/        #   Simplex, LU factorization, Branch & Bound
├── velo/           # Routing Engine (libvelo.a)
│   └── src/        #   OSM PBF parsing, Dijkstra, A*, landmarks
├── carta/          # Tile Generator (libcarta.a)
│   └── src/        #   MVT encoding, PNG rendering, Web Mercator
├── fuelwise/       # Refueling Library (libfuelwise.a)
│   └── src/        #   LP formulation, route filtering
├── api/            # REST API Server
├── wasm/           # WebAssembly Builds
├── ui/             # React Application
├── docs/           # Documentation
├── examples/       # Usage Examples
└── tests/          # Integration Tests
```

## Module Details

### Ralph - LP/MIP Solver

Zero-dependency linear programming solver using the Revised Simplex method with LU factorization and Branch & Bound for mixed-integer problems.

```c
#include "ralph.h"

RalphModel *model = ralph_create();
ralph_add_variable(model, 0, 100, 5.0);  // x: 0 ≤ x ≤ 100, cost 5
ralph_add_constraint(model, ...);
ralph_optimize(model);
double x = ralph_get_solution(model, 0);
ralph_free(model);
```

**Tests:** 43 | **Performance:** 1000+ variable problems

### Velo - Routing Engine

OSM PBF routing with multiple algorithms and preprocessing optimizations.

```c
#include "velo.h"

VLGraph *graph = vl_load_pbf("map.osm.pbf");
VLRoute route;
vl_route_coords(graph, origin, destination, NULL, &route);
printf("Distance: %.1f km\n", route.distance_m / 1000);
vl_graph_free(graph);
```

**Tests:** 30+ | **Performance:** Country-scale routing in <100ms

### Carta - Map Tile Generator

Generate vector (MVT) and raster (PNG) tiles from OSM data.

```c
#include "carta.h"

CTPBFContext *ctx = ct_load_pbf("map.osm.pbf");
CTTileCoord tile = {14, 9058, 5729};  // z/x/y

// Vector tile
size_t mvt_size = ct_generate_mvt(ctx, tile, NULL, buffer, capacity);

// Raster tile
size_t png_size = ct_generate_png(ctx, tile, NULL, NULL, buffer, capacity);

ct_free_pbf_context(ctx);
```

**Tests:** 33 | **Performance:** ~75ms per 512x512 PNG tile

### FuelWise - Refueling Optimization

Optimal fueling strategy using LP/MILP formulation.

```c
#include "fuelwise.h"

FWProblem problem = {
    .stations = stations,
    .num_stations = 10,
    .tank_capacity = 300,
    .current_fuel = 50,
    .consumption_rate = 0.15  // gal/mile
};

FWRefuelSolution solution;
fw_solve_refuel_lp(&problem, &solution);
printf("Total cost: $%.2f\n", solution.total_cost);
```

**Tests:** 29 | **Performance:** <100ms typical optimization

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| POST | `/api/v1/filter` | Filter stations to route corridor |
| POST | `/api/v1/solve` | Solve refueling optimization |
| POST | `/api/v1/optimize` | Full pipeline (filter + solve) |

### Example

```bash
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{
    "stations": [
      {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
      {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.45}
    ],
    "route": [[34.05, -118.24], [33.45, -112.07]],
    "tank_capacity": 300,
    "current_fuel": 50,
    "consumption_mpg": 6.5
  }'
```

## Build Commands

```bash
# All libraries
make all            # ralph + fuelwise + velo + carta

# Individual modules
make ralph          # LP/MIP solver
make fuelwise       # Refueling library
make velo           # Routing engine
make carta          # Tile generator
make api            # REST API server
make wasm           # WebAssembly builds

# Testing
make test           # All tests
make test-ralph     # Ralph tests (43)
make test-fuelwise  # FuelWise tests (29)
make test-velo      # Velo tests (30+)
make test-carta     # Carta tests (33)

# Run
make run-api        # Start API on :8080
```

## Requirements

- **Build:** GCC/Clang (C11), GNU Make
- **WASM:** Emscripten (optional)
- **UI:** Node.js 20+ (optional)
- **Maps:** OSM PBF files from [Geofabrik](https://download.geofabrik.de/)

## Performance

| Operation | Time | Notes |
|-----------|------|-------|
| LP solve (1000 vars) | <100ms | Ralph simplex |
| Route (country-scale) | <100ms | Velo A* bidirectional |
| PNG tile (512x512) | ~75ms | Carta rasterizer |
| Refuel optimization | <100ms | FuelWise LP |
| PBF parse (Hungary) | ~10s | 300MB, 35M nodes |

## Documentation

### Core Libraries
- [Architecture Overview](docs/ARCHITECTURE.md)
- [REST API Reference](docs/API.md)
- [Ralph Solver](ralph/CLAUDE.md) - LP/MIP optimization
- [Velo Routing](velo/CLAUDE.md) - OSM routing engine
- [Carta Tiles](carta/CLAUDE.md) - Map tile generation
- [FuelWise Library](fuelwise/CLAUDE.md) - Refueling domain

### Vendor Libraries
- [Miniz](vendor/miniz/CLAUDE.md) - zlib-compatible compression
- [Mongoose](vendor/mongoose/CLAUDE.md) - Embedded HTTP server
- [Clay](vendor/clay/CLAUDE.md) - UI layout library

## Design Principles

1. **Zero Dependencies** - Core libraries use only standard C
2. **WASM-First** - All components compile to WebAssembly
3. **Layered Architecture** - Clear separation: solver → domain → API → UI
4. **Portable** - Runs on Linux, macOS, Windows, browsers

## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

Mark Farkas - 2025

---

**Ralph** - **R**obust **A**I **L**inear **P**rogramming **H**elper
**Velo** - **V**ery **E**fficient **L**ocation **O**ptimizer
**Carta** - **C**ompact **A**gile **R**endering for **T**ile **A**rchives
