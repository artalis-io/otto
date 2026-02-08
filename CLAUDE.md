# Claude Code Instructions for OTTO Platform

## Overview

**OTTO** is a trucking logistics optimization platform in C with TypeScript React frontends. All core libraries compile to WebAssembly with zero external dependencies.

## Quick Reference

| Task | Command |
|------|---------|
| Build all | `make all` |
| Run tests | `make test` |
| Start servers | Use `/api-run` skill |
| Audit C code | Use `/c-audit` skill |
| Audit JS code | Use `/js-audit` skill |
| Audit ClayShards | Use `/clayshards-audit` skill |

## Components

| Component | Location | Port | Purpose |
|-----------|----------|------|---------|
| Ralph | `ralph/` | - | LP/MIP solver |
| Velo | `velo/` | 8082 | OSM routing |
| Carta | `carta/` | 8081 | Map tiles (MVT/PNG) |
| Locus | `locus/` | 8083 | Geocoding |
| FuelWise | `fuelwise/` | 8080 | Refueling optimization |
| ClayShards | `clayshards/` | - | Immediate mode UI |
| Shared | `shared/` | - | Geo, protobuf, rate limiting |

## Key Files by Task

### LP/MIP Optimization (Ralph)
```
ralph/include/ralph.h      # Solver API
ralph/src/simplex.c        # Revised Simplex (critical)
ralph/src/lu.c             # LU factorization (most sensitive)
ralph/src/branch_bound.c   # MIP solver
```

### Routing (Velo)
```
velo/include/velo.h        # Routing API
velo/src/vl_route.c        # Dijkstra, A*, bidirectional
velo/src/vl_graph.c        # CSR graph structure
velo/src/vl_landmarks.c    # ALT algorithm
```

### Map Tiles (Carta)
```
carta/include/carta.h      # Tile generation API
carta/src/ct_mvt.c         # MVT protobuf encoding
carta/src/ct_render.c      # PNG rasterization
carta/src/ct_tile.c        # Web Mercator math
```

### Geocoding (Locus)
```
locus/include/locus.h      # Geocoding API
locus/src/lc_trie.c        # Prefix trie for autocomplete
locus/src/lc_ngram.c       # Trigram fuzzy search
locus/src/lc_spatial.c     # Grid-based reverse geocoding
```

### Refueling (FuelWise)
```
fuelwise/include/fuelwise.h  # Library API
fuelwise/src/fw_refuel.c     # LP/MILP formulation
fuelwise/src/fw_route.c      # Station filtering
```

### Shared Infrastructure
```
shared/include/sh_geo.h        # Coordinate types, Haversine
shared/include/sh_ratelimit.h  # Token bucket rate limiter
shared/include/sh_workqueue.h  # Bounded work queue
shared/include/sh_capacity.h   # M/M/c queue planning
shared/include/sh_args.h       # CLI/env arg parsing
shared/include/sh_circuit.h    # Circuit breaker
shared/include/sh_backoff.h    # Exponential backoff
shared/include/sh_protobuf.h   # Protobuf primitives
```

### UI System (ClayShards)
```
clayshards/clay-shards/include/cs_common.h           # Core API
clayshards/clay-shards/src/cs_map.c                  # Map component
clayshards/clay-shards-webgl/renderer.js             # WebGL renderer
clayshards/clay-shards-webgl/map-provider.js         # API client
clayshards/clay-shards-tui/include/cs_tui.h          # TUI renderer API
clayshards/clay-shards-tui/src/cs_tui.c              # Terminal ANSI renderer
clayshards/clay-shards-tui-webgl/tui-renderer.js     # TUI WebGL renderer
clayshards/clay-shards-tui-webgl/crt-effects.js      # CRT post-processing
```

## Naming Conventions

| Scope | Pattern | Example |
|-------|---------|---------|
| Ralph functions | `ralph_*` | `ralph_optimize()` |
| Velo functions | `vl_*` | `vl_route_astar()` |
| Carta functions | `ct_*` | `ct_generate_png()` |
| Locus functions | `lc_*` | `lc_search()` |
| FuelWise functions | `fw_*` | `fw_solve_refuel_lp()` |
| Shared functions | `sh_*` | `sh_haversine()` |
| ClayShards functions | `cs_*` | `cs_button()` |
| Types | `PascalCase` | `VLGraph`, `CTPBFContext` |
| Constants | `UPPER_SNAKE` | `VL_PROFILE_TRUCK` |

## C Memory Safety Patterns

### Arena Allocation (Preferred)
```c
Arena arena = arena_create(buffer, size);
Node *nodes = arena_alloc(&arena, n * sizeof(Node));
/* ... use nodes ... */
arena_reset(&arena);  /* Free everything at once */
```

### Ownership Rules
```c
/* _create() implies _destroy() */
Graph *g = graph_create();
graph_destroy(g);
g = NULL;  /* Prevent use-after-free */
```

### Buffer Safety
```c
/* NEVER: strcpy, sprintf, gets */
/* ALWAYS: strncpy, snprintf with size */
snprintf(buf, sizeof(buf), "%s", str);
```

### Integer Overflow
```c
/* Check before multiply */
if (count > SIZE_MAX / element_size) return NULL;
void *p = calloc(count, element_size);  /* Or use calloc */
```

## Common Pitfalls

1. **Ralph**: RHS must be non-negative for constraints
2. **Velo**: Bidirectional A* needs consistent heuristic
3. **Carta**: Coordinate order is (lon, lat) in MVT
4. **Locus**: Queries must be UTF-8 encoded
5. **FuelWise**: Stations must be sorted by distance_from_start
6. **Memory**: Always free: solutions, routes, contexts

## Test Counts

```
ralph:    73 tests
fuelwise: 33 tests
shared:   137 tests (includes circuit, backoff, retry)
velo:     47 tests
carta:    33 tests
locus:    52 tests
clayshards: 101 tests (includes TUI renderer)
```

## Build Commands

```bash
# Libraries
make all              # Build all + tests
make {ralph,velo,carta,locus,fuelwise,shared}

# API Servers
make {carta,velo,locus,fuelwise}-api

# WASM (requires Emscripten)
make wasm

# Testing
make test             # All tests
make test-{ralph,velo,carta,locus,fuelwise,shared}
make test-{carta,velo,locus,fuelwise}-api

# API Documentation
make api-docs         # Generate site/api.html (auto-rebuilds dependencies)
make api-docs-check   # Verify docs are up-to-date (for CI)
```

### API Documentation Generation

`site/api.html` is generated from C header annotations (`/*@api ... */`) and includes live WASM demos with embedded Monaco data.

**Dependency chain** (all automatic):
```
data/monaco-latest.osm.pbf  (downloads if missing)
           ↓
data/monaco.vlg  (rebuilds when velo/ sources change)
           ↓
{velo,carta}/wasm/src/monaco_*.h  (embedded data headers)
           ↓
{velo,carta}/wasm/build/*-api-demo.js  (WASM modules)
           ↓
site/api.html  (copies WASM to site/js/, generates HTML)
```

**Key files:**
- `scripts/build-api-docs.py` - Generator script (parses headers, renders template)
- `site/api-template.html` - HTML template with Jinja2-like syntax
- `site/api-config.json` - Module configuration (ports, WASM settings)
- `{module}/include/*.h` - API annotations parsed for docs

**Force full rebuild:**
```bash
rm -f data/monaco.vlg velo/wasm/src/monaco_vlg.h carta/wasm/src/monaco_pbf.h
make api-docs
```

### Site Build Structure

The `site/` directory contains source files; `site/build/` contains deployable files:

```bash
make site-build   # Build site/build/
make site-serve   # Build and serve on :8000
```

**What's in site/build/** (deployed):
- `index.html`, `api.html`, `style.css`
- `js/*.js` (WASM wrappers, typing animation)
- `wasm/*.js` (WASM modules)

**What stays in site/** (not deployed):
- `api-template.html`, `api-config.json` (generator sources)
- `Makefile`, `README.md` (build/docs)

## Performance Targets

| Operation | Target |
|-----------|--------|
| LP solve (1000 vars) | <100ms |
| Route (country-scale) | <100ms |
| PNG tile (512x512) | ~75ms |
| Geocoding | <20µs |

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Applications: React / WASM / REST APIs             │
├─────────────────────────────────────────────────────┤
│  Fleet: HoSE, Tempo, Arbor, Surge, Sigma, Pulse [Planned]  │
│  Network: Atlas, Quota [Planned]                    │
├─────────────────────────────────────────────────────┤
│  State: Fuse (signal fusion, geofencing) [Planned]  │
├─────────────────────────────────────────────────────┤
│  Domain: FuelWise │ Velo │ Carta │ Locus            │
├─────────────────────────────────────────────────────┤
│  Core: Ralph (LP/MIP) │ Shared (geo, proto, rate)   │
├─────────────────────────────────────────────────────┤
│  Vendor: miniz │ mongoose │ Clay                    │
└─────────────────────────────────────────────────────┘
```

**Fuse** provides confidence-weighted vehicle state to all engines. Geofencing is part of Fuse (requires position confidence for reliable triggers).

## Transport-Agnostic API Design

OTTO APIs follow a transport-agnostic pattern. Core logic is pure C functions; HTTP/WASM/embedded are thin wrappers.

```
┌─────────────────────────────────────────────────────┐
│  Transport Layer (thin, ~10 lines each)             │
│  Mongoose HTTP │ WASM+JS │ Unix socket │ Embedded   │
├─────────────────────────────────────────────────────┤
│  Core API (pure C functions)                        │
│  carta_render_tile() │ vl_route() │ lc_search()     │
└─────────────────────────────────────────────────────┘
```

**Pattern:**
```c
// Core: transport-agnostic, runs anywhere
int carta_render_tile(int z, int x, int y, uint8_t **out, size_t *len);

// HTTP wrapper: parse request → call core → format response
// WASM wrapper: parse JSON → call core → return JSON
// Embedded: direct call
```

**Benefits:**
- **Demo IS the product** - Browser WASM runs the actual algorithms
- **Zero-infrastructure eval** - Single HTML file, no server needed
- **Edge-ready by design** - If it runs in WASM, it runs anywhere

See `docs/MANIFESTO.md` for the full manifesto.

## Skills Reference

| Skill | Purpose |
|-------|---------|
| `/api-run` | Start servers, CLI args, env vars |
| `/c-audit` | C code security review |
| `/js-audit` | JavaScript/WebGL code review |
| `/clayshards-audit` | Full ClayShards audit (C + JS + Manifesto) |

## Documentation Structure

| Path | Purpose |
|------|---------|
| `docs/ARCHITECTURE.md` | System architecture, layers, data flow |
| `docs/MANIFESTO.md` | Design philosophy (C, WASM, transport-agnostic, render-agnostic) |
| `docs/KNOWN_ISSUES.md` | Known bugs and workarounds |
| `docs/TOOLING.md` | Build tools, scripts, CI |
| `docs/business/` | Strategy, valuation (some content has redaction markers) |
| `docs/roadmaps/` | Active development roadmaps by component (15 files) |
| `docs/archive/` | Completed or superseded plans |

### Roadmaps

Active roadmaps in `docs/roadmaps/`:

| File | Components |
|------|------------|
| `velo.md` | Continental routing, CCH, landmarks |
| `carta.md` | Labels, styling, MVT optimizations |
| `forge.md` | Async job queue architecture |
| `nexus.md` | TMS/ELD integration gateway |
| `hose.md` | Hours of Service engine |
| `tempo.md` | Time windows, business rules |
| `arbor.md` | State-space search |
| `sigma.md` | Fleet plan selection (MIP) |
| `pulse.md` | Execution tracker, PTA engine |
| `atlas.md` | Network design |
| `quota.md` | Pricing engine |

See `docs/roadmaps/README.md` for priority overview.

### Technical Internals

Deep-dive documentation in `docs/internals/`:

| File | Topic |
|------|-------|
| `ralph-architecture.md` | LP/MIP solver architecture |
| `lu-factorization.md` | LU decomposition, eta updates |
| `simplex.md` | Revised simplex implementation |
| `clayshards-design.md` | UI widget state, focus model |

## Vendor Libraries

| Library | Location | Purpose |
|---------|----------|---------|
| miniz | `vendor/miniz/` | zlib compression |
| mongoose | `vendor/mongoose/` | HTTP server |
| Clay | `vendor/clay/` | UI layout |

Each has its own `CLAUDE.md` with API documentation.
