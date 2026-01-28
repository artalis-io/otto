# FuelWise Platform

**Intelligent Truck Refueling & Route Optimization**

A complete platform for truck fleet optimization, combining route planning, fuel cost minimization, and custom map rendering. Built entirely in C with zero external dependencies, designed for native and WebAssembly deployment.

## Platform Components

| Component | Location | Description |
|-----------|----------|-------------|
| [**Ralph**](ralph/) | `ralph/` | Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound) |
| [**Velo**](velo/) | `velo/` | OSM routing engine (Dijkstra, A*, bidirectional, landmarks) |
| [**Carta**](carta/) | `carta/` | Map tile generator (MVT vector tiles, PNG raster) |
| [**FuelWise**](fuelwise/) | `fuelwise/` | Refueling optimization library |
| [**Shared**](shared/) | `shared/` | Common geo utilities |
| FuelWise API | `fuelwise/api/` | REST API server for refueling optimization |
| Route Server | `velo/api/` | REST API server for routing |
| Tile Server | `carta/api/` | REST API server for map tiles |
| FuelWise UI | `fuelwise/ui/` | React application with map interface |
| FuelWise WASM | `fuelwise/wasm/` | WebAssembly builds for browser deployment |

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

# Run all tests (190+ tests across all modules)
make test

# Build and start API servers
make api            # FuelWise API (fuelwise/api)
make tile-server    # Carta Tile Server (carta/api)
make route-server   # Velo Route Server (velo/api)

# Start servers
make run-api        # FuelWise API on :8080

# Build and run UI
cd fuelwise/ui && npm install && npm run dev
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         FuelWise Platform                               │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐                   │
│  │  React UI   │   │    WASM     │   │  REST APIs  │   Applications    │
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
│  │  │  miniz   │  │ Protobuf │  │ mongoose │  │  shared  │         │   │
│  │  │  (zlib)  │  │ (decode) │  │  (http)  │  │  (geo)   │         │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘         │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

## Project Structure

```
fuelwise-platform/
├── ralph/              # LP/MIP Solver (libralph.a)
│   ├── src/            #   Simplex, LU factorization, Branch & Bound
│   ├── tests/          #   Solver tests + debug utilities
│   └── docs/           #   Simplex, LU documentation
├── velo/               # Routing Engine (libvelo.a)
│   ├── src/            #   OSM PBF parsing, Dijkstra, A*, landmarks
│   └── api/            #   Route server REST API
├── carta/              # Tile Generator (libcarta.a)
│   ├── src/            #   MVT encoding, PNG rendering, Web Mercator
│   └── api/            #   Tile server REST API + Leaflet UI
├── fuelwise/           # Refueling Library (libfuelwise.a)
│   ├── src/            #   LP formulation, route filtering
│   ├── api/            #   FuelWise REST API
│   ├── wasm/           #   WebAssembly build
│   ├── ui/             #   React application
│   ├── examples/       #   Usage examples
│   ├── tests/          #   Domain tests + artifacts
│   └── docs/           #   API documentation
├── shared/             # Shared Utilities (libshared.a)
├── vendor/             # Third-party libraries
│   ├── mongoose/       #   HTTP server
│   ├── miniz/          #   zlib compression
│   └── clay/           #   UI layout (future)
├── docs/               # Architecture documentation
└── docker/             # Docker configuration
```

## API Endpoints

### FuelWise API (:8080)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| POST | `/api/v1/filter` | Filter stations to route corridor |
| POST | `/api/v1/solve` | Solve refueling optimization |
| POST | `/api/v1/optimize` | Full pipeline (filter + solve) |

### Velo Route Server (:8082)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| GET | `/api/v1/stats` | Graph statistics |
| GET/POST | `/api/v1/route` | Calculate route with profile |

### Carta Tile Server (:8081)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| GET | `/api/v1/stats` | PBF statistics |
| GET | `/tiles/{z}/{x}/{y}.png` | Raster tile (PNG) |
| GET | `/tiles/{z}/{x}/{y}.mvt` | Vector tile (MVT) |
| GET | `/tiles.json` | TileJSON metadata |

### Example

```bash
# Refueling optimization
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

# Route calculation
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=car&mode=fastest'

# Map tile
curl 'http://localhost:8081/tiles/14/9058/5729.png' > tile.png
```

## Build Commands

```bash
# Libraries
make all              # Build all with tests (default)
make lib              # Build libraries only (no tests)
make ralph            # LP/MIP solver
make fuelwise         # Refueling library
make shared           # Shared utilities
make velo             # Routing engine
make carta            # Tile generator

# API Servers
make api              # FuelWise API (fuelwise/api)
make route-server     # Velo route server (velo/api)
make tile-server      # Carta tile server (carta/api)

# WebAssembly (requires Emscripten)
make wasm             # Build all WASM modules
make wasm-fuelwise    # FuelWise WASM only
make wasm-velo        # Velo WASM only
make wasm-carta       # Carta WASM only
make wasm-types       # Generate TypeScript declarations
make wasm-test        # Test WASM builds

# Testing
make test             # All tests (~190)
make test-ralph       # Ralph tests (65)
make test-fuelwise    # FuelWise tests (32)
make test-shared      # Shared tests (23)
make test-velo        # Velo tests (39)
make test-carta       # Carta tests (33)

# Run servers
make run-api          # Start FuelWise API on :8080
make run-tiles        # Show tile server usage
make run-routes       # Show route server usage
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
| Route (country-scale) | <100ms | Velo A* with landmarks |
| PNG tile (512x512) | ~75ms | Carta rasterizer |
| Refuel optimization | <100ms | FuelWise LP |
| PBF parse (Hungary) | ~10s | 300MB, 35M nodes |

## Documentation

### Core Libraries
- [Architecture Overview](docs/ARCHITECTURE.md)
- [Ralph Solver](ralph/CLAUDE.md) - LP/MIP optimization
- [Velo Routing](velo/CLAUDE.md) - OSM routing engine
- [Carta Tiles](carta/CLAUDE.md) - Map tile generation
- [FuelWise Library](fuelwise/CLAUDE.md) - Refueling domain

### API Servers
- [FuelWise API](fuelwise/api/CLAUDE.md) - Refueling REST API
- [FuelWise API Reference](fuelwise/docs/API.md) - Endpoint documentation
- [Velo Route Server](velo/api/CLAUDE.md) - Routing REST API
- [Carta Tile Server](carta/api/CLAUDE.md) - Tile server REST API

### Vendor Libraries
- [Miniz](vendor/miniz/CLAUDE.md) - zlib-compatible compression
- [Mongoose](vendor/mongoose/CLAUDE.md) - Embedded HTTP server
- [Clay](vendor/clay/CLAUDE.md) - UI layout library

## Design Principles

1. **Zero Dependencies** - Core libraries use only standard C
2. **WASM-First** - All components compile to WebAssembly
3. **Layered Architecture** - Clear separation: solver -> domain -> API -> UI
4. **Portable** - Runs on Linux, macOS, Windows, browsers

## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

Mark Farkas - 2025

---

**Ralph** - **R**obust **A**I **L**inear **P**rogramming **H**elper
**Velo** - **V**ery **E**fficient **L**ocation **O**ptimizer
**Carta** - **C**ompact **A**gile **R**endering for **T**ile **A**rchives
