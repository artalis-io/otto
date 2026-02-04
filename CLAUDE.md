# Claude Code Instructions for OTTO Platform

## Overview

**OTTO** is a trucking logistics optimization platform in C with TypeScript React frontends. All core libraries compile to WebAssembly with zero external dependencies.

## Quick Reference

| Task | Command |
|------|---------|
| Build all | `make all` |
| Run tests | `make test` |
| Start servers | Use `/api-servers` skill |
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
clayshards/clay-shards/include/cs_common.h     # Core API
clayshards/clay-shards/src/cs_map.c            # Map component
clayshards/clay-shards-webgl/renderer.js       # WebGL renderer
clayshards/clay-shards-webgl/map-provider.js   # API client
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
clayshards: 84 tests
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
```

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
│  Domain: FuelWise │ Velo │ Carta │ Locus            │
├─────────────────────────────────────────────────────┤
│  Core: Ralph (LP/MIP) │ Shared (geo, proto, rate)   │
├─────────────────────────────────────────────────────┤
│  Vendor: miniz │ mongoose │ Clay                    │
└─────────────────────────────────────────────────────┘
```

## Skills Reference

| Skill | Purpose |
|-------|---------|
| `/api-servers` | Start servers, CLI args, env vars |
| `/c-audit` | C code security review |
| `/js-audit` | JavaScript/WebGL code review |
| `/clayshards-audit` | Full ClayShards audit (C + JS + Manifesto) |

## Documentation Links

- `docs/ARCHITECTURE.md` - System architecture
- `docs/STRATEGY.md` - Business strategy
- `docs/TODO_FEATURES.md` - Planned components (HoSE, Tempo, Arbor, etc.)

## Vendor Libraries

| Library | Location | Purpose |
|---------|----------|---------|
| miniz | `vendor/miniz/` | zlib compression |
| mongoose | `vendor/mongoose/` | HTTP server |
| Clay | `vendor/clay/` | UI layout |

Each has its own `CLAUDE.md` with API documentation.
