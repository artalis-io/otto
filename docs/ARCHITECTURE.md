# OTTO Platform Architecture

## Overview

OTTO (**O**ptimization for **T**rucking and **T**ransport **O**perations) is a comprehensive trucking logistics optimization platform built on a layered architecture of zero-dependency C libraries. The platform is designed for both native deployment and WebAssembly (WASM) for browser-based applications.

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
│                       UI Components                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                        ClayShards                                     │   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────────┐     │   │
│  │  │   Core     │  │   WebGL    │  │    TUI     │  │ TUI-WebGL  │     │   │
│  │  │ Components │  │  Renderer  │  │  Renderer  │  │ CRT Effects│     │   │
│  │  └────────────┘  └────────────┘  └────────────┘  └────────────┘     │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                             │                                                │
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
otto/
├── ralph/              # LP/MIP Solver (libralph.a)
│   ├── include/        #   Public headers
│   ├── src/            #   Simplex, LU, B&B, LAP, Network Flow, Detect
│   ├── api/            #   Ralph Solver REST API server
│   └── tests/          #   LP/MIP, LAP, Network Flow, Detect tests
│
├── fuelwise/           # Refueling Library (libfuelwise.a)
│   ├── include/        #   Public headers
│   ├── src/            #   LP formulation, route filtering
│   ├── tests/          #   Unit tests
│   ├── api/            #   FuelWise REST API server
│   ├── wasm/           #   WebAssembly build
│   └── ui/             #   React frontend application
│
├── velo/               # Routing Engine (libvelo.a)
│   ├── include/        #   Public headers
│   ├── src/            #   OSM PBF parsing, Dijkstra, A*, landmarks
│   ├── tests/          #   Unit tests
│   ├── api/            #   Velo Route Server REST API
│   └── wasm/           #   WebAssembly build
│
├── carta/              # Tile Generator (libcarta.a)
│   ├── include/        #   Public headers
│   ├── src/            #   MVT encoding, PNG rendering, Web Mercator
│   ├── tests/          #   Unit tests
│   ├── api/            #   Carta Tile Server REST API
│   ├── ui/             #   Tile Viewer React Application
│   └── wasm/           #   WebAssembly build
│
├── locus/              # Geocoding Engine (liblocus.a)
│   ├── include/        #   Public headers (locus.h, lc_*.h)
│   ├── src/            #   Trie, n-gram, spatial index, PBF parsing
│   ├── tests/          #   Unit tests
│   ├── api/            #   Locus Geocoding Server REST API
│   ├── tools/          #   CLI search utilities
│   └── wasm/           #   WebAssembly build
│
├── shared/             # Shared utilities (libshared.a)
│   ├── include/        #   Common headers
│   ├── src/            #   Geo utilities, protobuf, rate limiting, capacity
│   └── tests/          #   Unit tests
│
├── forge/              # Async Job Queue [PLANNED]
│   ├── include/        #   Public headers
│   ├── src/            #   Broker, dispatcher, SQLite persistence
│   ├── api/            #   REST + WebSocket server (:8085)
│   └── consumers/      #   Built-in job consumers
│
├── clayshards/         # UI Component System
│   ├── clay-shards/    #   Immediate-mode UI components (C11)
│   ├── clay-shards-webgl/    # WebGL renderer
│   ├── clay-shards-tui/      # Terminal TUI renderer
│   └── clay-shards-tui-webgl/# CRT effects for browser TUI
│
├── vendor/             # Third-party code (vendored)
│   ├── miniz/          #   Public domain zlib implementation
│   ├── mongoose/       #   Embedded HTTP server
│   ├── sqlite/         #   Embedded SQL database [PLANNED]
│   └── clay/           #   UI layout library
│
├── scripts/            # Utility scripts (prefix: build-, ci-, data-, demo-, test-, util-)
│   ├── data-download-osm.sh  # Download OSM PBF from Geofabrik
│   ├── test-benchmark.sh     # Performance benchmarks
│   └── ci-pipeline.sh        # CI/CD pipeline
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
**API Server:** `ralph/api/` (port 8084)
**Dependencies:** None (zero-dependency)

Core optimization engine implementing:
- **Simplex:** Revised Simplex Method with LU factorization
- **MIP:** Branch and Bound with Gomory cutting planes
- **LAP:** Linear Assignment Problem solver (JVC algorithm, O(n³))
  - Dense, sparse, rectangular, k-best, bottleneck variants
  - Priority, cardinality, and qualification constraints
- **Network Flow:** Network simplex for minimum cost flow
  - Warm start, bottleneck, cost scaling
- **Detection:** Auto-detects LAP/network structure in LP models
- Sparse matrix operations (CSC format)

See [internals/ralph-architecture.md](internals/ralph-architecture.md) for detailed solver internals.

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
- Binary index format (.lcx) for fast loading

Data structures:
- **Prefix Trie**: O(m) lookup for autocomplete (m = query length)
- **Trigram Index**: Jaccard similarity for fuzzy matching
- **Spatial Grid**: Cell-based geographic index for reverse geocoding

The GIS trifecta (**Velo**, **Carta**, **Locus**) provides complete geographic functionality:
- **Velo**: Routing (A* search, turn-by-turn navigation)
- **Carta**: Map tiles (vector MVT, raster PNG)
- **Locus**: Geocoding (forward search, autocomplete, reverse lookup)

### ClayShards - UI Component System
**Location:** `clayshards/`
**Dependencies:** `vendor/clay`

Immediate-mode UI component library in C11, built on Clay for layout. Renderer-agnostic design allows the same UI code to run on multiple backends:

- **clay-shards/**: Core components (buttons, inputs, checkboxes, sliders, dropdowns, scrolls, maps)
- **clay-shards-webgl/**: WebGL renderer for browser deployment
- **clay-shards-tui/**: Terminal TUI renderer (ANSI escape sequences)
- **clay-shards-tui-webgl/**: Browser TUI with CRT phosphor effects

Key principles:
- Immediate mode: UI rebuilt each frame, no retained widget tree
- Stable identity via string hash (`CS_ID("name")`)
- Renderer-agnostic: same code targets WebGL, TUI, or framebuffer
- State ownership: app owns business state, ClayShards owns UI state

See [MANIFESTO.md](MANIFESTO.md) for design philosophy.

### Shared - Common Utilities
**Location:** `shared/`
**Library:** `libshared.a`
**Dependencies:** None

Common code used by velo, carta, and API servers:
- Haversine distance calculation
- Coordinate projections (Web Mercator)
- Protobuf varint encoding/decoding
- Rate limiting (token bucket, IPv4/IPv6)
- Work queue (bounded, thread-safe)
- Capacity planning (queuing theory)

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

Generic job broker for long-running async tasks. See [roadmaps/forge.md](roadmaps/forge.md) for full specification.

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
make test             # Run all tests
make clean            # Clean all build artifacts
```

### API Server Targets
```makefile
make fuelwise-api     # Build FuelWise REST API (fuelwise/api) :8080
make carta-api        # Build Carta tile server (carta/api) :8081
make velo-api         # Build Velo route server (velo/api) :8082
make locus-api        # Build Locus geocoding server (locus/api) :8083
make ralph-api        # Build Ralph solver server (ralph/api) :8084
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
make test             # Run all tests
make test-ralph       # Ralph LP/MIP tests
make test-ralph-lap   # Ralph LAP solver tests
make test-ralph-netflow # Ralph network flow tests
make test-ralph-detect  # Ralph problem detection tests
make test-fuelwise    # FuelWise tests
make test-velo        # Velo routing tests
make test-carta       # Carta tile tests
make test-locus       # Locus geocoding tests
make test-shared      # Shared library tests
make test-api         # All API endpoint tests
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
7. **Defense in Depth**: Production-ready with multiple protective layers

## Production Hardening

API servers (Carta, Velo, Locus) implement defense-in-depth with three protective layers:

### Rate Limiting (sh_ratelimit.h)

Token bucket rate limiter at the IP level:

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP Request Flow                        │
├─────────────────────────────────────────────────────────────┤
│  Client Request                                             │
│        │                                                    │
│        ▼                                                    │
│  ┌───────────────┐                                         │
│  │ Rate Limiter  │ ←─ Per-IP token bucket                  │
│  │ (sh_ratelimit)│                                         │
│  └───────┬───────┘                                         │
│          │ Allowed?                                        │
│          ├── No  → HTTP 429 Too Many Requests              │
│          │                                                  │
│          ▼ Yes                                             │
│  ┌───────────────┐                                         │
│  │  Work Queue   │ ←─ Bounded buffer                       │
│  │(sh_workqueue) │                                         │
│  └───────┬───────┘                                         │
│          │ Space?                                          │
│          ├── No  → HTTP 503 Service Unavailable            │
│          │                                                  │
│          ▼ Yes                                             │
│  ┌───────────────┐                                         │
│  │Render Workers │ ←─ Thread pool                          │
│  │  (N threads)  │                                         │
│  └───────┬───────┘                                         │
│          │ Timeout?                                        │
│          ├── Yes → HTTP 504 Gateway Timeout                │
│          │                                                  │
│          ▼ No                                              │
│     HTTP 200 + Response                                    │
└─────────────────────────────────────────────────────────────┘
```

**Configuration:**
- `rate_limit_rps`: Requests per second per IP (token refill rate)
- `rate_limit_burst`: Maximum burst capacity (initial tokens)
- Supports IPv4 and IPv6 addresses
- Thread-safe with fine-grained locking

### Work Queue (sh_workqueue.h)

Bounded producer-consumer queue for backpressure:

- **Queue depth**: Maximum pending requests
- **Timeout**: Request expiration (stale requests rejected)
- Decouples HTTP handlers from CPU-intensive rendering
- Provides load shedding under pressure

**Stats exposed via `/api/v1/stats`:**
```json
{
  "work_queue": {
    "enabled": true,
    "depth": 42,
    "capacity": 100,
    "pushed": 12345,
    "popped": 12300,
    "dropped": 5,
    "expired": 2
  }
}
```

### Capacity Planning (sh_capacity.h)

Queuing theory utilities for optimal configuration:

```c
ShCapacityParams params;
sh_capacity_calculate(&params, &(ShCapacityInput){
    .avg_response_ms = 75,        // Measured average response time
    .num_workers = 8,             // Number of worker threads
    .target_utilization = 0.7,    // 70% utilization target
    .client_timeout_ms = 10000,   // Client gives up after 10s
    .burst_tiles = 25             // Tiles in initial map view
});
// params now contains recommended rate_limit_rps, burst, queue_depth, timeout
```

**Key formulas (M/M/c queue model):**
- Service rate: μ = 1000 / avg_response_ms (requests/sec/worker)
- Max throughput: c × μ × target_utilization
- Queue depth: sized to drain within client timeout
- Burst capacity: accommodates initial map view load

**Configuration validation:**
```c
int warnings = sh_capacity_validate(
    current_queue_depth, current_timeout, current_rate_limit,
    measured_response_ms, num_workers,
    warning_buf, sizeof(warning_buf)
);
// Returns 0 if configuration is sane, >0 with warning messages
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CARTA_RATE_LIMIT_RPS` | 10.0 | Token refill rate per IP |
| `CARTA_RATE_LIMIT_BURST` | 100.0 | Initial/max tokens per IP |
| `CARTA_WORK_QUEUE_ENABLED` | 1 | Enable work queue (0 = sync) |
| `CARTA_WORK_QUEUE_DEPTH` | 100 | Max pending requests |
| `CARTA_WORK_QUEUE_TIMEOUT` | 10.0 | Request timeout (seconds) |
| `CARTA_RENDER_WORKERS` | auto | Render thread pool size |

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

## Remaining Structural TODOs

- [ ] **Standardize test structure** - Adopt single pattern across all modules
- [ ] **Add integration tests** - End-to-end tests (route + refuel + tiles)
- [ ] **Add CONTRIBUTING.md and CHANGELOG.md**
- [ ] **Generate TypeScript types** - `gen-types.sh` from C headers
