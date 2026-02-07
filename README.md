# OTTO

**O**ptimization for **T**rucking and **T**ransport **O**perations

A comprehensive trucking and logistics optimization platform combining route planning, fuel cost minimization, Hours of Service compliance, fleet scheduling, and custom map rendering. Built entirely in C with zero external dependencies, designed for native and WebAssembly deployment.

## Platform Components

### Core Engines

| Component | Description | Port |
|-----------|-------------|------|
| [**Ralph**](ralph/) | Zero-dependency LP/MIP solver (Revised Simplex, Branch & Bound) | - |
| [**Velo**](velo/) | OSM routing engine (Dijkstra, A*, bidirectional, landmarks) | 8082 |
| [**Carta**](carta/) | Map tile generator (MVT vector, PNG raster) | 8081 |
| [**Locus**](locus/) | OSM geocoding (forward search, autocomplete, reverse lookup) | 8083 |
| [**FuelWise**](fuelwise/) | Refueling optimization library | 8080 |
| [**ClayShards**](clayshards/) | Immediate mode UI components (WebGL, TUI renderers) | - |
| [**Shared**](shared/) | Common utilities: geo, protobuf, zlib, rate limiting | - |

### Planned Engines

| Component | Description |
|-----------|-------------|
| **Fuse** | State integrity (**F**leet **U**nified **S**ignal and **E**stimation) - multi-source fusion, vehicle identity, confidence-weighted geofencing |
| **HoSE** | Hours of Service (**H**ours **o**f **S**ervice **E**ngine) - FMCSA/EC561 |
| **Tempo** | Time windows and business rules (**T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator) |
| **Arbor** | State-space search engine (**A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime) |
| **Sigma** | Fleet plan selection via MIP (**S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation) |
| **Pulse** | Execution tracker / PTA engine (**P**lan **U**tilization and **L**ive **S**tate **E**stimator) |
| **Forge** | Async job queue (**F**lexible **O**rchestration and **R**untime for **G**eneral **E**xecution) |
| **Apex** | Pre-computation engine (**A**synchronous **P**re-computation **Ex**ecution) - tiles, routes, cache |
| **Nexus** | TMS/ELD/LoadBoard integration gateway (**N**ormalized **Ex**ternal **U**nified **S**napshots) |
| **Iris** | LLM natural language interface (**I**ntelligent **R**equest **I**nterpretation **S**ystem) |
| **Quota** | Pricing engine (**Q**uote **U**nderwriting and **T**ariff **O**ptimization **A**lgorithm) |
| **Atlas** | Network design (**A**llocation and **T**actical **L**ane **A**nalysis **S**ystem) |

## Quick Start

```bash
# Build everything
make all

# Run tests (~340 tests)
make test

# Start API servers
./carta/api/carta-tile-server data/hungary-latest.osm.pbf    # Tiles on :8081
./velo/api/velo-route-server data/hungary-latest.osm.pbf     # Routes on :8082
./locus/api/locus-geocoder data/hungary-latest.osm.pbf       # Geocoding on :8083
./fuelwise/api/fuelwise-api                                   # Optimization on :8080

# Download OSM data
./scripts/download-osm.sh hungary   # ~300MB
./scripts/download-osm.sh monaco    # ~1MB (for testing)
```

### Docker

```bash
docker-compose up                       # Full platform
docker-compose --profile all-apis up    # All API servers
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Applications: React UI / WASM (Browser) / REST APIs / CLI      │
├─────────────────────────────────────────────────────────────────┤
│  Fleet Optimization [Planned]: HoSE, Tempo, Arbor, Sigma, Pulse │
│  Network Analysis [Planned]: Atlas, Quota                       │
├─────────────────────────────────────────────────────────────────┤
│  State Integrity [Planned]: Fuse (signal fusion, geofencing)    │
├─────────────────────────────────────────────────────────────────┤
│  Domain Libraries: FuelWise, Velo, Carta, Locus                 │
├─────────────────────────────────────────────────────────────────┤
│  Core: Ralph (LP/MIP) │ Shared (geo, protobuf, rate limiting)   │
├─────────────────────────────────────────────────────────────────┤
│  Vendor: miniz (zlib) │ mongoose (HTTP) │ Clay (UI layout)      │
└─────────────────────────────────────────────────────────────────┘
```

**Fuse** is the state integrity layer that provides confidence-weighted vehicle positions to all other engines. Geofencing, detention tracking, and ETA stability all depend on Fuse's unified reality model.

## API Examples

```bash
# Route calculation
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=truck'

# Map tile
curl 'http://localhost:8081/tiles/14/9058/5729.png' -o tile.png

# Geocoding
curl 'http://localhost:8083/api/v1/search?q=Budapest'

# Refueling optimization
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{"stations":[...],"route":[...],"tank_capacity":300}'
```

## Performance

| Operation | Time | Notes |
|-----------|------|-------|
| LP solve (1000 vars) | <100ms | Ralph simplex |
| Route (country-scale) | <100ms | Velo A* + landmarks |
| PNG tile (512x512) | ~75ms | Carta rasterizer |
| Geocoding | <20µs | Locus trie + ngram |
| PBF parse (Hungary) | ~10s | 300MB, 35M nodes |

## Documentation

- **[CLAUDE.md](CLAUDE.md)** - Development guide and patterns
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** - System architecture
- **[docs/business/STRATEGY.md](docs/business/STRATEGY.md)** - Business strategy
- **[site/api.html](site/api.html)** - Interactive API documentation with WASM demos

### Regenerating API Documentation

The API documentation (`site/api.html`) is generated from C header annotations and includes live WASM demos. It auto-rebuilds when sources change:

```bash
make api-docs              # Generate site/api.html (rebuilds dependencies as needed)
make api-docs-check        # Verify docs are up-to-date (for CI)
```

**Dependency chain** (all automatic):
```
data/monaco-latest.osm.pbf  →  data/monaco.vlg  →  WASM demos  →  site/api.html
         ↓                           ↓                  ↓
   (downloads if missing)    (rebuilds if velo/ changes)  (copies to site/js/)
```

To force a full rebuild:
```bash
rm -f data/monaco.vlg velo/wasm/src/monaco_vlg.h carta/wasm/src/monaco_pbf.h
make api-docs
```

### Building & Serving the Site

The `site/` directory contains source files. Deployable files are built into `site/build/`:

```bash
make site-build            # Build site/build/ (only deployable files)
make site-serve            # Build and serve on :8000
```

**Structure:**
- `site/` - Source files (templates, config, Makefile)
- `site/build/` - Deployable files only (gitignored, auto-generated)

### Component Documentation

Each component has a `CLAUDE.md` with API details:
- [Ralph](ralph/CLAUDE.md) | [Velo](velo/CLAUDE.md) | [Carta](carta/CLAUDE.md) | [Locus](locus/CLAUDE.md)
- [FuelWise](fuelwise/CLAUDE.md) | [ClayShards](clayshards/clay-shards/CLAUDE.md) | [Shared](shared/README.md)

## Design Philosophy

- **Zero Dependencies** - Core libraries use only standard C
- **WASM-First** - All components compile to WebAssembly (50-150KB)
- **Transport-Agnostic APIs** - Core is pure C functions; HTTP/WASM/embedded are thin wrappers
- **C11** - Compound literals, designated initializers, no runtime overhead
- **Arena Allocators** - Bulk alloc, bulk free, no fragmentation
- **Immediate Mode UI** - No retained state, deterministic rendering

### Transport-Agnostic Architecture

Every OTTO API is fundamentally a pure C function:

```c
int carta_render_tile(int z, int x, int y, uint8_t **out, size_t *len);
int vl_route(VLGraph *g, int from, int to, VLRoute *route);
```

HTTP (Mongoose) and WASM (Emscripten) are just thin wrappers (~10 lines each) over the same core. This means:

- **The demo IS the product** - Browser WASM demos run the actual algorithms
- **Zero-infrastructure evaluation** - Send a single HTML file, no server needed
- **Edge-ready by design** - If it runs in WASM, it runs anywhere

See [docs/MANIFESTO.md](docs/MANIFESTO.md) for the full manifesto.

### Render Backend Agnostic UI

ClayShards applies the same philosophy to the frontend. UI code produces render commands—WebGL, TUI terminal, OpenGL ES are thin renderers over the same component code:

```c
// Hybrid design: declarative layout (Clay) + imperative interaction (ClayShards)
CLAY(CLAY_ID("Panel"), CLAY_LAYOUT(.padding = {16, 16, 16, 16})) {
    if (cs_button(CS_ID("submit"), "Submit", NULL).clicked) {
        handle_submit();  // Immediate mode: if clicked, react now
    }
}
// Layout answers "where and how big?" — Components answer "what does it do?"
```

- **Declarative layout** - Clay handles sizing, padding, flex layouts
- **Imperative interaction** - Immediate mode widgets respond to input inline
- **Render commands as contract** - UI produces commands; renderers consume them
- **TUI as strictest target** - If it works in terminal, it works everywhere

See [clayshards/clay-shards/MANIFESTO.md](clayshards/clay-shards/MANIFESTO.md) for the full manifesto.

See [README_DESIGN.md](docs/MANIFESTO.md) for detailed rationale.

## Requirements

- **Build:** GCC/Clang (C11), GNU Make
- **WASM:** Emscripten (optional)
- **UI:** Node.js 20+ (optional)
- **Maps:** OSM PBF files from [Geofabrik](https://download.geofabrik.de/)

## License

MIT License - see [LICENSE](LICENSE)

---

**OTTO** - Optimization for Trucking and Transport Operations
