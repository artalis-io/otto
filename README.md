# OTTO

**O**ptimization for **T**rucking and **T**ransport **O**perations

A comprehensive trucking and logistics optimization platform, combining route planning, fuel cost minimization, Hours of Service compliance, fleet scheduling, and custom map rendering. Built entirely in C with zero external dependencies, designed for native and WebAssembly deployment.

## Platform Components

### Core Engines (Active)

| Component | Location | Description |
|-----------|----------|-------------|
| [**Ralph**](ralph/) | `ralph/` | Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound) |
| [**Velo**](velo/) | `velo/` | OSM routing engine (Dijkstra, A*, bidirectional, landmarks) |
| [**Carta**](carta/) | `carta/` | Map tile generator (MVT vector tiles, PNG raster) |
| [**Locus**](locus/) | `locus/` | OSM geocoding engine (forward search, autocomplete, reverse lookup) |
| [**FuelWise**](fuelwise/) | `fuelwise/` | Refueling optimization library |
| [**Shared**](shared/) | `shared/` | Common geo utilities, protobuf, zlib |

### Planned Engines

| Component | Location | Description |
|-----------|----------|-------------|
| **Forge** | `forge/` | **F**lexible **O**rchestration and **R**untime for **G**eneral **E**xecution - async job queue |
| **HoSE** | `hose/` | **H**ours **o**f **S**ervice **E**ngine - FMCSA/EC561 compliance |
| **Tempo** | `tempo/` | **T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator |
| **Arbor** | `arbor/` | **A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime |
| **Sigma** | `sigma/` | **S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation |
| **Pulse** | `pulse/` | **P**lan **U**tilization and **L**ive **S**tate **E**stimator |
| **Nexus** | `nexus/` | **N**ormalized **Ex**ternal **U**nified **S**napshots - TMS/ELD/LoadBoard integration |

### Applications

| Component | Location | Description |
|-----------|----------|-------------|
| FuelWise API | `fuelwise/api/` | REST API server for refueling optimization |
| Route Server | `velo/api/` | REST API server for routing |
| Tile Server | `carta/api/` | REST API server for map tiles |
| Geocoding Server | `locus/api/` | REST API server for geocoding |
| FuelWise UI | `fuelwise/ui/` | React application with map interface |
| Carta UI | `carta/ui/` | Tile viewer React application |
| FuelWise WASM | `fuelwise/wasm/` | WebAssembly builds for browser deployment |
| Locus WASM | `locus/wasm/` | Geocoding WebAssembly for browser |

### UI System

| Component | Location | Description |
|-----------|----------|-------------|
| [**ClayShards**](shared/ui/clay-shards/) | `shared/ui/clay-shards/` | Immediate mode UI components (button, input, map) |
| [**ClayShards WebGL**](shared/ui/clay-shards-webgl/) | `shared/ui/clay-shards-webgl/` | WebGL renderer for browsers |
| [**ClayShards Demo**](shared/ui/clay-shards-demo/) | `shared/ui/clay-shards-demo/` | Example map viewer application |

## Quick Start

### Using Docker

```bash
# Full platform (UI + API)
docker build -t otto .
docker run -p 80:80 -p 8080:8080 otto

# API only
docker build --target api-only -t otto-api .
docker run -p 8080:8080 otto-api

# Multi-service with Docker Compose
docker-compose up                       # Full platform
docker-compose --profile all-apis up    # All 3 API servers
docker-compose --profile dev up         # Development environment
```

### Building from Source

```bash
# Build all C libraries
make all

# Run all tests (190+ tests across all modules)
make test

# Build API servers
make fuelwise-api   # FuelWise API (fuelwise/api)
make carta-api      # Carta Tile Server (carta/api)
make velo-api       # Velo Route Server (velo/api)

# Start FuelWise API server
make run-fuelwise-api   # FuelWise API on :8080

# Build and run UI
cd fuelwise/ui && npm install && npm run dev
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              OTTO Platform                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐  ┌─────────────┐ │
│  │  React +    │   │    WASM     │   │  REST APIs  │  │    Nexus    │ │
│  │ ClayShards  │   │  (Browser)  │   │ (mongoose)  │  │ TMS/ELD/LB  │ │
│  └──────┬──────┘   └──────┬──────┘   └──────┬──────┘  └─────[plan'd]┘ │
│         └─────────────────┼─────────────────┘                          │
│                           │                                             │
│  ┌────────────────────────┴────────────────────────────────────────┐   │
│  │                 Fleet Optimization (Planned)                     │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐        │   │
│  │  │   HoSE   │  │  Tempo   │  │  Arbor   │  │  Sigma   │  Pulse │   │
│  │  │   HoS    │  │  Time    │  │  Search  │  │  Fleet   │  PTA   │   │
│  │  │  Rules   │  │ Windows  │  │  Tree    │  │ Planning │  Track │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘        │   │
│  └────────────────────────────────────────────────────────────────┘   │
│                           │                                             │
│  ┌────────────────────────┴────────────────────────────────────────┐   │
│  │                         Domain Libraries                         │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────┐│   │
│  │  │   FuelWise   │  │     Velo     │  │    Carta     │  │Locus ││   │
│  │  │   Refueling  │  │   Routing    │  │  Map Tiles   │  │Geo-  ││   │
│  │  │ Optimization │  │   Engine     │  │  Generator   │  │code  ││   │
│  │  └──────┬───────┘  └──────────────┘  └──────────────┘  └──────┘│   │
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
│  │  │  miniz   │  │ Protobuf │  │ mongoose │  │  shared  │  Forge  │   │
│  │  │  (zlib)  │  │ (decode) │  │  (http)  │  │  (geo)   │  (jobs) │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘ [plan'd]│   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

## Project Structure

```
otto/
├── ralph/              # LP/MIP Solver (libralph.a)
│   ├── src/            #   Simplex, LU factorization, Branch & Bound
│   ├── tests/          #   Solver tests + debug utilities
│   └── docs/           #   Simplex, LU documentation
├── velo/               # Routing Engine (libvelo.a)
│   ├── src/            #   OSM PBF parsing, Dijkstra, A*, landmarks
│   └── api/            #   Route server REST API
├── carta/              # Tile Generator (libcarta.a)
│   ├── src/            #   MVT encoding, PNG rendering, Web Mercator
│   ├── api/            #   Tile server REST API
│   └── ui/             #   Tile viewer React application
├── fuelwise/           # Refueling Library (libfuelwise.a)
│   ├── src/            #   LP formulation, route filtering
│   ├── api/            #   FuelWise REST API
│   ├── wasm/           #   WebAssembly build
│   ├── ui/             #   React application
│   ├── examples/       #   Usage examples
│   ├── tests/          #   Domain tests + artifacts
│   └── docs/           #   API documentation
├── locus/              # Geocoding Engine (liblocus.a)
│   ├── src/            #   Trie, ngram, spatial index
│   ├── api/            #   Geocoding REST API
│   └── wasm/           #   WebAssembly build
├── shared/             # Shared Utilities (libshared.a)
│   └── ui/             #   UI System
│       ├── clay-shards/        # Immediate mode components
│       ├── clay-shards-webgl/  # WebGL renderer
│       └── clay-shards-demo/   # Demo map viewer
├── forge/              # [Planned] Async Job Queue
├── hose/               # [Planned] Hours of Service Engine
├── tempo/              # [Planned] Business Rules Engine
├── arbor/              # [Planned] State-Space Search Engine
├── sigma/              # [Planned] Fleet Plan Selection Engine
├── pulse/              # [Planned] Execution Tracker / PTA Engine
├── nexus/              # [Planned] External Data Integration Gateway
├── vendor/             # Third-party libraries
│   ├── mongoose/       #   HTTP server
│   ├── miniz/          #   zlib compression
│   └── clay/           #   UI layout (future)
├── scripts/            # Utility scripts
│   ├── download-osm.sh #   Download OSM data from Geofabrik
│   ├── benchmark.sh    #   Performance benchmarks
│   └── ci.sh           #   CI/CD pipeline
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

### Locus Geocoding Server (:8083)

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| GET | `/api/v1/stats` | Index statistics |
| GET | `/api/v1/search` | Forward geocoding (text to coordinates) |
| GET | `/api/v1/autocomplete` | Prefix-based autocomplete |
| GET | `/api/v1/reverse` | Reverse geocoding (coordinates to address) |

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

# Geocoding
curl 'http://localhost:8083/api/v1/search?q=Budapest'
curl 'http://localhost:8083/api/v1/autocomplete?q=Buda'
curl 'http://localhost:8083/api/v1/reverse?lat=47.5&lon=19.0'
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
make locus            # Geocoding engine

# API Servers
make fuelwise-api     # FuelWise API (fuelwise/api)
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
make fuelwise-ui-dev  # Run FuelWise UI dev server on :5173
make carta-ui         # Build Carta Tile Viewer
make carta-ui-dev     # Run Carta UI dev server

# Testing
make test             # All library tests (~279)
make test-ralph       # Ralph tests (73)
make test-fuelwise    # FuelWise tests (33)
make test-shared      # Shared tests (41)
make test-velo        # Velo tests (47)
make test-carta       # Carta tests (33)
make test-locus       # Locus tests (52)
make test-api         # All API endpoint tests (requires OSM data)
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

## Scripts

```bash
# Download OSM data
./scripts/download-osm.sh hungary    # Download Hungary (~300MB)
./scripts/download-osm.sh monaco     # Download Monaco (~1MB, for testing)
./scripts/download-osm.sh list       # List available regions

# Performance benchmarks
./scripts/benchmark.sh               # Run all benchmarks
./scripts/benchmark.sh velo          # Benchmark routing only
./scripts/benchmark.sh carta         # Benchmark tile generation only

# CI/CD pipeline
./scripts/ci.sh                      # Run full CI pipeline
./scripts/ci.sh quick                # Quick build + test
./scripts/ci.sh lint                 # Lint only
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
| Forward geocoding | <20µs | Locus trie + ngram |
| Autocomplete | <20µs | Locus prefix search |
| Reverse geocoding | <30µs | Locus spatial index |

## Documentation

### Core Libraries
- [Architecture Overview](docs/ARCHITECTURE.md)
- [Ralph Solver](ralph/CLAUDE.md) - LP/MIP optimization
- [Velo Routing](velo/CLAUDE.md) - OSM routing engine
- [Carta Tiles](carta/CLAUDE.md) - Map tile generation
- [Locus Geocoding](locus/CLAUDE.md) - OSM geocoding engine
- [FuelWise Library](fuelwise/CLAUDE.md) - Refueling domain

### API Servers
- [FuelWise API](fuelwise/api/CLAUDE.md) - Refueling REST API
- [FuelWise API Reference](fuelwise/docs/API.md) - Endpoint documentation
- [Velo Route Server](velo/api/CLAUDE.md) - Routing REST API
- [Carta Tile Server](carta/api/CLAUDE.md) - Tile server REST API
- [Locus Geocoding Server](locus/api/CLAUDE.md) - Geocoding REST API

### UI System
- [ClayShards](shared/ui/clay-shards/CLAUDE.md) - Immediate mode UI components
- [ClayShards WebGL](shared/ui/clay-shards-webgl/) - WebGL renderer
- [ClayShards Demo](shared/ui/clay-shards-demo/CLAUDE.md) - Example map viewer

### Vendor Libraries
- [Miniz](vendor/miniz/CLAUDE.md) - zlib-compatible compression
- [Mongoose](vendor/mongoose/CLAUDE.md) - Embedded HTTP server
- [Clay](vendor/clay/CLAUDE.md) - UI layout library

## Design Philosophy

### Why C11 (Moving to C17)?

OTTO is written almost entirely in **C11**, with plans to adopt **C17** features as compiler support matures. This choice is deliberate and stems from several key advantages:

#### Language Features That Matter

| Feature | C11 | Why It Matters for OTTO |
|---------|-----|-------------------------|
| **Compound literals** | `(Type){.field = val}` | Inline struct construction for styles, configs, state |
| **Designated initializers** | `{.x = 1, .y = 2}` | Self-documenting, order-independent initialization |
| **Anonymous structs/unions** | `struct { struct { int x; }; }` | Cleaner nested state hierarchies |
| **`_Static_assert`** | Compile-time checks | Catch size/alignment errors at build time |
| **`_Generic`** | Type-generic macros | Safe type dispatch without C++ templates |
| **`_Alignas/_Alignof`** | Alignment control | SIMD-friendly data layouts |
| **Thread-local** | `_Thread_local` | Per-thread state without locking |

#### Compound Literals as First-Class Concepts

Compound literals are central to OTTO's API design. They enable:

```c
/* Inline configuration - no separate variable declaration */
cs_button(CS_ID("save"), "Save", &(CsButtonStyle){
    .variant = CS_BTN_PRIMARY,
    .font_size = 14,
    .corner_radius = 4
});

/* State initialization with clear defaults */
static AppState g_app = {
    .map = {
        .lat = 47.4979,
        .lon = 19.0402,
        .zoom = 12,
    },
    .panels = {
        .show_tile_info = true,
        .layer_type = 0,
    },
};

/* Theme as typed constant (not preprocessor macros) */
static const struct {
    Clay_Color bg_dark;
    Clay_Color text_light;
    Clay_Color border;
} THEME = {
    .bg_dark    = {40, 40, 40, 230},
    .text_light = {255, 255, 255, 255},
    .border     = {100, 100, 100, 255},
};
```

**Benefits over older C:**
- No need for factory functions or builder patterns
- Values are self-documenting (named fields vs positional)
- Compiler enforces type correctness
- Easy to add fields with defaults (unlisted = zero)

#### Why Not Other Languages?

| Language | Why Not |
|----------|---------|
| **C++** | 200KB+ WASM (exceptions, RTTI), slow compile, complex ABI |
| **Rust** | 150KB+ WASM baseline, steep learning curve, slower iteration |
| **Zig** | Immature ecosystem, unstable language spec, limited tooling |
| **Go** | 2MB+ WASM (runtime + GC), no fine-grained memory control |
| **Java** | No viable WASM path, GC pauses, massive runtime |
| **TypeScript** | GC overhead, no SIMD, can't match native perf for algorithms |

**Detailed Analysis:**

**C++ (Considered, Rejected)**
```
Pros: RAII, templates, STL
Cons: Exception handling adds 50-100KB to WASM
      Name mangling breaks FFI simplicity
      Header-heavy codebases compile slowly
      Template errors are cryptic
```
C++ features like RAII are valuable, but OTTO's hot paths (LP solving, routing, tile generation) benefit more from explicit control. We'd disable exceptions anyway, losing half the value proposition.

**Rust (Considered, Rejected)**
```
Pros: Memory safety, excellent WASM tooling, fearless concurrency
Cons: 150KB+ baseline WASM (core library, panic handling)
      Borrow checker friction for graph algorithms
      Slower prototyping than C
      Async/await adds complexity we don't need
```
Rust is excellent for systems programming, but OTTO's algorithms are simpler than Rust's safety model assumes. The borrow checker fights graph-heavy code (routing, LP basis). Our C code is small enough to audit manually.

**Zig (Interesting, Too Early)**
```
Pros: C interop, comptime, no hidden control flow
Cons: Language still changing (0.x versions)
      Limited library ecosystem
      IDE/debugger support immature
      Small community for help
```
Zig's philosophy aligns with OTTO's, but betting on a pre-1.0 language for production code is risky. May revisit when Zig stabilizes.

**Go (Wrong Fit)**
```
Pros: Fast compilation, good tooling, easy concurrency
Cons: 2MB+ WASM (Go runtime + garbage collector)
      GC pauses unacceptable for real-time tile rendering
      No manual memory layout control
      Generics arrived late, ecosystem hasn't adapted
```
Go's runtime overhead makes it unsuitable for performance-critical WASM. A "Hello World" Go WASM is 2MB; our entire map viewer is 136KB.

**Java/JVM (Non-Starter)**
```
Pros: Mature ecosystem, excellent tooling
Cons: No production WASM path (TeaVM/CheerpJ are experiments)
      GC pauses in hot loops
      JVM memory overhead (100MB+ baseline)
      Primitive type boxing overhead
```
Java's VM model is incompatible with WASM's goals. The JVM is optimized for long-running servers, not embedded/browser contexts.

**TypeScript/JavaScript (Used for UI, Not Core)**
```
Pros: Ubiquitous in browsers, rapid development
Cons: GC pauses during animation frames
      No SIMD (WebAssembly has it, JS doesn't)
      JIT warmup latency
      Can't match C for numerical algorithms
```
We use TypeScript for React UIs where it excels. Core algorithms stay in C/WASM where microseconds matter. The boundary is clean: JS handles DOM/events, WASM handles computation.

**The C Sweet Spot**

For OTTO's requirements (LP solving, routing, tile generation, WASM deployment):

| Requirement | C Advantage |
|-------------|-------------|
| **Small WASM** | 50-150KB typical, no runtime |
| **Predictable perf** | No GC, no JIT, no surprises |
| **Memory control** | Arena allocators, cache-friendly layouts |
| **FFI simplicity** | Direct function exports, no marshaling |
| **Compile speed** | Seconds, not minutes |
| **Debuggability** | Simple stack traces, printf works |

C11 gives us 90% of the ergonomics we'd want (compound literals, designated initializers) without the complexity tax of newer languages.

### Why Zero Dependencies?

Every external dependency is a liability. OTTO's core libraries depend only on:
- Standard C library (`<stdlib.h>`, `<string.h>`, `<math.h>`, `<stdint.h>`)
- Optional: Emscripten for WASM builds

#### What "Zero Dependencies" Enables

| Benefit | Explanation |
|---------|-------------|
| **Deterministic builds** | Same source = same binary, always |
| **Auditable code** | Every line can be inspected |
| **Minimal attack surface** | No supply chain vulnerabilities |
| **Fast compilation** | No dependency resolution or linking |
| **WASM size control** | Include only what you use |
| **Cross-platform** | No platform-specific deps |

#### The Cost (And Why It's Worth It)

We implement some things ourselves that libraries provide:
- **LP solver** (Ralph) - 3000 lines vs pulling GLPK/CLP
- **Routing** (Velo) - 2000 lines vs pulling OSRM
- **Compression** (miniz) - Single-file zlib alternative

This code is:
- Tailored to our exact needs
- Fully understood by the team
- Modifiable without upstream coordination
- Distributable without license concerns

### Why WebAssembly (WASM)?

WASM is a first-class deployment target, not an afterthought.

#### WASM Advantages

| Property | Benefit |
|----------|---------|
| **Sandboxed execution** | Can't access filesystem or network directly |
| **Near-native speed** | LP solving in the browser at 80-90% native perf |
| **Portable bytecode** | Same binary runs everywhere |
| **No installation** | Link in HTML, instant deployment |
| **JavaScript interop** | Seamless FFI with typed arrays |
| **Streaming compilation** | Start running while downloading |

#### Design Constraints for WASM Compatibility

To compile cleanly to WASM, OTTO follows these rules:

1. **No threads in hot paths** - WASM threads have limitations
2. **Fixed-size allocations** - Arena allocators, no unbounded malloc
3. **Explicit exports** - `EMSCRIPTEN_KEEPALIVE` on public functions
4. **No filesystem assumptions** - Data passed via memory, not files
5. **No global constructors** - Explicit `_init()` functions

```c
/* WASM-compatible export pattern */
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

EXPORT int solve_lp(double *c, int n, ...) {
    /* Same code runs native and in browser */
}
```

### Module Design Considerations

#### Layered Architecture

```
┌───────────────────────────────────────────────┐
│  Applications (UI, API, CLI)                  │  ← User-facing
├───────────────────────────────────────────────┤
│  Domain Libraries (FuelWise, HoSE, Tempo)     │  ← Business logic
├───────────────────────────────────────────────┤
│  Core Engines (Ralph, Velo, Carta)            │  ← Algorithms
├───────────────────────────────────────────────┤
│  Shared Utilities (geo, projections)          │  ← Common code
└───────────────────────────────────────────────┘
```

**Rules:**
- Lower layers never depend on higher layers
- Each layer exposes a clean C API
- State is passed explicitly (no hidden globals)

#### State Management Pattern

OTTO uses a single `AppState` structure per application:

```c
/* All mutable state in one place */
typedef struct {
    MapState map;       /* Geographic viewport */
    UIPanels panels;    /* UI visibility flags */
    UIText text;        /* Input buffers */
    ClayState clay;     /* Layout engine state */
} AppState;

/* Initialized with compound literal */
static AppState g_app = {
    .map = { .lat = 47.5, .lon = 19.0, .zoom = 12 },
    .panels = { .show_tile_info = true },
};
```

**Benefits:**
- Clear ownership and lifetime
- Easy to serialize/deserialize
- Simple debugging (inspect one struct)
- No hidden state scattered across files

#### Component API Design

Components follow the immediate mode pattern:

```c
/* Call the component, check the result, react */
CsButtonResult r = cs_button(CS_ID("save"), "Save", &style);
if (r.clicked) {
    save_document();
}
```

**Properties:**
- No retained state outside explicit buffers
- Result is fully determined by inputs
- Caller owns all data (text buffers, selection state)
- Components are pure functions + global interaction state

### UI System Design

OTTO's UI combines two paradigms:

| Layer | Style | Purpose |
|-------|-------|---------|
| **Clay** | Declarative | Layout computation (flexbox-like) |
| **cs_*** | Immediate mode | Interaction handling (ClayShards) |

```c
/* Declarative layout wraps immediate mode interaction */
CLAY(CLAY_ID("Panel"), {
    .layout = { .padding = CLAY_PADDING_ALL(16) },
    .backgroundColor = THEME.bg_dark,
}) {
    /* Immediate mode component inside declarative container */
    if (cs_button(CS_ID("btn"), "Click", NULL).clicked) {
        handle_click();
    }
}
```

**Why hybrid?**
- Clay handles complex layout efficiently
- Immediate mode is intuitive for interactions
- Best of both worlds without the downsides

### Future Renderer Backends

The renderer-agnostic design supports multiple backends:

| Backend | Target | Status |
|---------|--------|--------|
| `clay-shards-webgl` | Browsers | Active |
| `clay-shards-sdl` | Desktop/Mobile | Planned |
| `clay-shards-raylib` | Games | Planned |
| `clay-shards-sokol` | Minimal deps | Planned |
| `clay-shards-terminal` | TUI | Planned |

Each renderer implements:
- Rectangle drawing (solid, rounded, borders)
- Text rendering with font metrics callback
- Texture/image rendering
- Scissor/clipping regions

### Performance Guidelines

| Principle | Implementation |
|-----------|----------------|
| **Minimize allocations** | Arena allocators, pre-sized arrays |
| **Cache-friendly layout** | Structs of arrays for hot data |
| **Branch prediction** | Common paths first in conditionals |
| **SIMD-ready** | Aligned data, vectorizable loops |
| **Lazy computation** | Compute on access, not on change |

### Error Handling Strategy

```c
/* Errors are values, not exceptions */
typedef struct {
    bool ok;
    const char *error;  /* NULL if ok */
} Result;

Result do_something(void) {
    if (bad_condition) {
        return (Result){ .ok = false, .error = "description" };
    }
    return (Result){ .ok = true };
}

/* Caller checks explicitly */
Result r = do_something();
if (!r.ok) {
    log_error(r.error);
    return;
}
```

**No hidden control flow. No exceptions. No surprises.**

### C Memory Safety Practices

C lacks Rust's borrow checker, but disciplined patterns prevent the common vulnerability classes. OTTO uses these strategies throughout:

#### 1. Arena Allocators (Preferred)

Most OTTO code uses **arena allocation** - bulk allocate, bulk free:

```c
/* Allocate arena once */
uint8_t memory[1024 * 1024];
Arena arena = arena_create(memory, sizeof(memory));

/* All allocations come from arena */
Node *nodes = arena_alloc(&arena, count * sizeof(Node));
Edge *edges = arena_alloc(&arena, edge_count * sizeof(Edge));

/* Single reset frees everything - no individual free() calls */
arena_reset(&arena);  /* All nodes, edges freed atomically */
```

**Why this works:**
- **No double-free** - Individual items are never freed
- **No dangling pointers** - Everything freed together at known point
- **No fragmentation** - Contiguous allocation, single reset
- **Cache-friendly** - Related data is contiguous in memory

**Used in:** Ralph (LP solver), Velo (routing), Carta (tile generation)

#### 2. Ownership Rules

When arena allocation isn't suitable, follow explicit ownership:

```c
/* RULE: Creator owns, creator frees */
Solution *sol = solver_solve(problem);  /* solver_solve allocates */
/* ... use sol ... */
solution_free(sol);                      /* Caller must free */
sol = NULL;                              /* Prevent use-after-free */

/* RULE: _create() implies _destroy() */
Graph *g = graph_create();
/* ... */
graph_destroy(g);  /* Symmetric naming = clear ownership */
g = NULL;
```

#### 3. Defensive Patterns

```c
/* Pattern: NULL after free */
void safe_free(void **ptr) {
    if (ptr && *ptr) {
        free(*ptr);
        *ptr = NULL;  /* Prevent double-free and use-after-free */
    }
}

/* Pattern: Check before use */
if (ptr == NULL) {
    return ERROR_NULL_POINTER;
}

/* Pattern: Validate indices */
if (index < 0 || index >= array_len) {
    return ERROR_OUT_OF_BOUNDS;
}
```

#### 4. Buffer Overflow Prevention

```c
/* NEVER: unbounded copy */
strcpy(dest, src);           /* NO - buffer overflow */
sprintf(buf, "%s", str);     /* NO - buffer overflow */

/* ALWAYS: bounded operations */
strncpy(dest, src, sizeof(dest) - 1);
dest[sizeof(dest) - 1] = '\0';  /* Ensure null termination */

snprintf(buf, sizeof(buf), "%s", str);  /* Auto-truncates */

/* BETTER: Use explicit length tracking */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} String;
```

#### 5. Integer Overflow Prevention

```c
/* DANGEROUS: Unchecked multiplication for allocation */
void *p = malloc(count * element_size);  /* Can overflow! */

/* SAFE: Check before multiplication */
if (count > SIZE_MAX / element_size) {
    return ERROR_OVERFLOW;
}
void *p = malloc(count * element_size);

/* SAFE: Use calloc (checks internally on most platforms) */
void *p = calloc(count, element_size);
```

#### 6. Struct Initialization

```c
/* DANGEROUS: Uninitialized struct */
Config cfg;
use_config(&cfg);  /* Garbage values! */

/* SAFE: Zero initialization */
Config cfg = {0};  /* All fields zeroed */

/* SAFE: Designated initializers (explicit values) */
Config cfg = {
    .timeout = 30,
    .retries = 3,
    /* Unlisted fields are zero */
};
```

#### 7. WASM Sandboxing Bonus

WASM provides an additional safety layer:

```
┌─────────────────────────────────────────┐
│  WASM Linear Memory (Sandboxed)        │
│  - Cannot access host memory           │
│  - Bounds-checked by runtime           │
│  - No arbitrary pointer arithmetic     │
│  - Stack overflow = trap, not exploit  │
└─────────────────────────────────────────┘
```

Even if C code has a buffer overflow, WASM constrains the blast radius to the linear memory sandbox. This doesn't excuse sloppy code, but provides defense-in-depth.

#### 8. Static Analysis

OTTO uses these tools to catch issues early:

```bash
# Compiler warnings (treat as errors in CI)
gcc -Wall -Wextra -Werror -Wconversion -Wshadow

# Static analyzer
scan-build make

# Valgrind for native builds
valgrind --leak-check=full ./test_runner

# Address Sanitizer
gcc -fsanitize=address,undefined -g
```

#### Summary: OTTO's Memory Safety Checklist

| Category | Practice |
|----------|----------|
| **Allocation** | Prefer arena allocators; bulk alloc, bulk free |
| **Ownership** | `_create()` implies `_destroy()`; creator frees |
| **After free** | Set pointer to NULL immediately |
| **Strings** | Always use `snprintf`, never `sprintf` |
| **Indices** | Validate bounds before array access |
| **Integers** | Check for overflow before `malloc(a * b)` |
| **Structs** | Always initialize: `Type x = {0}` or designated |
| **Tools** | `-Wall -Wextra`, valgrind, ASan in CI |

## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

Mark Farkas - 2025

---

**OTTO** - **O**ptimization for **T**rucking and **T**ransport **O**perations

### Component Backronyms

| Component | Backronym |
|-----------|-----------|
| **Ralph** | **R**obust **A**I **L**inear **P**rogramming **H**elper |
| **Velo** | **V**ery **E**fficient **L**ocation **O**ptimizer |
| **Carta** | **C**ompact **A**gile **R**endering for **T**ile **A**rchives |
| **Locus** | **L**ocation **O**riented **C**oordinate **U**nification **S**ystem |
| **Forge** | **F**lexible **O**rchestration and **R**untime for **G**eneral **E**xecution |
| **HoSE** | **H**ours **o**f **S**ervice **E**ngine |
| **Tempo** | **T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator |
| **Arbor** | **A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime |
| **Sigma** | **S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation |
| **Pulse** | **P**lan **U**tilization and **L**ive **S**tate **E**stimator |
| **Nexus** | **N**ormalized **Ex**ternal **U**nified **S**napshots |
