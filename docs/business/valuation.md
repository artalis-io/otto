# OTTO Platform - Reproduction Cost Valuation

**Purpose:** Asset appraisal for capital contribution
**Method:** Cost-to-reproduce (engineering labor at market rate)
**Date:** February 2026 (revised)
**Basis:** Full codebase audit of ~297K LOC proprietary code

---

## Methodology

Reproduction cost = person-months required to recreate equivalent functionality from scratch, multiplied by loaded cost for engineers with requisite expertise (LP/MIP optimization, numerical linear algebra, systems programming, WebGL, WASM).

**Loaded cost used:** $15,000-$20,000/month per senior C/algorithms engineer (salary + benefits + overhead)

The assessment was performed by auditing every source file across all modules, reading key implementations, and evaluating algorithmic depth, numerical complexity, test coverage, and architectural sophistication.

---

## Codebase Size

| Module | LOC | Description |
|--------|-----|-------------|
| **Ralph** | 81,337 | LP/MIP solver (from feature branch with MIP infrastructure work) |
| **Shared** | 64,602 | Platform foundation libraries |
| **Carta** | 25,980 | Map tile renderer (MVT + PNG) |
| **ClayShards** | 23,187 | Immediate-mode UI system (C + JS) |
| **Velo** | 13,765 | OSM routing engine |
| **Site** | 12,611 | Landing page, API docs generator, WASM demos |
| **Locus** | 12,871 | Geocoding engine |
| **FuelWise** | 11,768 | Refueling optimizer |
| **Surge** | 7,511 | VRP/PDPTW solver |
| **Scripts** | 3,374 | CI/CD, orchestration, benchmarks |
| **Docs** | 40,324 | Architecture, manifesto, internals, roadmaps |
| | | |
| **Total proprietary** | **~297K LOC** | Excludes vendor (mongoose/miniz/clay ~46K) |

---

## Component Breakdown

### Ralph LP/MIP Solver — 81K LOC

**Estimated effort: 24-36 person-months**

The most complex and valuable component. A production-grade LP/MIP solver with:

| Subsystem | LOC | What it implements |
|-----------|-----|--------------------|
| Revised Simplex | 5,694 | Primal simplex with 5 pricing strategies (Dantzig, steepest edge, Devex, partial, heap), Harris ratio test, bound flipping, perturbation anti-cycling |
| Dual Simplex | 1,687 | Dual feasibility, bound-flip selection, steepest-edge pricing, Farkas ray extraction |
| LU Factorization | 6,209 | Sparse LU with AMD ordering, Markowitz pivot selection, supernodal variant, Forrest-Tomlin updates, eta-file updates, dynamic pivot tolerance, refactorization triggers |
| Presolve | 2,756 | 13 techniques (fixed vars, singletons, forcing constraints, bound tightening, redundant rows, etc.) with postsolve stack |
| Branch & Bound | 1,985 | Best-first/depth-first/hybrid node selection, strong branching, pseudo-cost learning, reliability branching |
| Cutting Planes | 2,591 | Gomory mixed-integer, MIR, SCP-specific clique/odd-hole/lifted-cover cuts, cut pool with aging |
| MIP Orchestration | 1,827 | Node/cut management, RINS heuristic, reduced-cost fixing, cut callbacks |
| Benders Decomposition | 1,486 | Automatic master/subproblem partitioning, optimality/feasibility cuts, Farkas ray validation |
| Network Simplex | 2,151 | Minimum cost network flow with tree-based O(nm) iterations |
| LAP Solver | 4,531 | Jonker-Volgenant-Castanon with SIMD vectorization and OpenMP parallelization |
| SCP Solver | — | Lagrangian relaxation, subgradient optimization, conflict graph, greedy repair |
| Structure Detection | 1,381 | Automatic LAP/SCP/SPP structure identification for solver specialization |
| File I/O | 3,632 | MPS and LP format readers/writers |
| Tests | 27,823 | 51 test files including GLPK comparison, presolve regression, deterministic failure tracing |

**Why this is hard to reproduce:**
- Numerical stability hardening (pivot recovery, phase-1 rescue, dynamic tolerance adjustment, growth factor tracking) represents years of iterative debugging
- Sparse LU with 3 factorization paths and 2 update methods is a textbook subject that takes significant effort to make production-reliable
- The combination of LP + MIP + specialized solvers (LAP, network, SCP, Benders) in a single zero-dependency package that compiles to WASM does not exist in the open-source ecosystem
- Comparable commercial solvers (Gurobi, CPLEX) have multi-decade development histories

### Velo Routing Engine — 14K LOC

**Estimated effort: 6-10 person-months**

| Subsystem | LOC | What it implements |
|-----------|-----|--------------------|
| Core Algorithms | 1,399 | Dijkstra, bidirectional Dijkstra, A*, bidirectional A* with consistent potentials, lazy timestamp initialization |
| ALT Landmarks | 630 | Farthest-first landmark selection, dual heuristics (distance + time), triangle inequality bounds, 2.5-3.8x speedup |
| Graph Construction | 1,794 | CSR format, reverse index, 1000x1000 grid spatial index, degree-2 contraction (40-60% node reduction), Hilbert curve reordering |
| OSM PBF Parsing | 708 | DenseNodes delta-decoding, highway classification, access restrictions, speed defaults |
| API Layer | 615 | Transport-agnostic request/response, polyline encoding |
| Tests | 2,249 | 51 unit tests + OSRM comparison suite |
| Benchmarks | 2,317 | Algorithm performance, route quality, PBF parsing speed |
| WASM | 580 | Browser routing with embedded Monaco graph |

**Performance:** Country-scale routing (Hungary, 2.7M nodes) in <100ms with landmarks.

### Carta Tile Renderer — 26K LOC

**Estimated effort: 8-12 person-months**

| Subsystem | LOC | What it implements |
|-----------|-----|--------------------|
| PBF Parsing | 2,772 | OSM feature extraction, tag classification, coordinate pools, hash maps |
| Software Rasterizer | 2,039 | Xiaolin Wu anti-aliased lines, scanline polygon fill, text rendering with halos, road casing |
| Label Placement | 900 | Priority-based collision detection, 9-anchor fallback, road labels with per-glyph rotation, area labels via polylabel |
| Metatile Cache | 994 | 2x2 tile groups for cross-boundary label consistency, LRU eviction, pthread rwlock thread safety |
| Tile Math | 958 | Web Mercator projection, geometry scaling/clipping, zoom-level calculations |
| MVT Encoding | 632 | Mapbox Vector Tile protobuf with delta + zigzag encoding, layer deduplication |
| R-tree Index | 505 | Hilbert-packed Sort-Tile-Recursive bulk loading, flat-array cache-friendly storage |
| Binary Serialization | 1,049 | Mmap-able index format for instant startup (<1s vs 10-60s PBF parsing) |
| Geometry Simplification | 509 | Iterative Douglas-Peucker (no stack overflow), zoom-adaptive tolerance |
| Multipolygon Assembly | 515 | OSM relation stitching, outer/inner ring detection, winding order validation |
| Styling Engine | 347 | 8 road types, 7 railway types, 10 landuse types, zoom-adaptive widths |
| PNG Encoding | 459 | RGBA with filter selection (None/Sub/Up/Average/Paeth), configurable compression |
| Polylabel | 218 | Pole of inaccessibility for area labels via iterative cell subdivision |
| Tests | 3,744 | PBF parsing, tile generation, label placement, rendering, metatile cache |
| API Server | 1,937 | REST tile server with TileJSON, Leaflet viewer, multi-threaded serving |

### ClayShards UI System — 23K LOC

**Estimated effort: 8-12 person-months**

| Subsystem | LOC | What it implements |
|-----------|-----|--------------------|
| C Core | 5,200 | Immediate-mode widgets (button, input, slider, dropdown, checkbox, scroll), focus model, persistent state store |
| Map Widget | 890 | Tile fetching, Web Mercator projection, pan/zoom, overlay support |
| WebGL Renderer | 2,800 | GLSL shaders, VAO/VBO management, tile rendering, font texture atlas |
| TUI Renderer | 1,000 | ANSI terminal rendering, color palette, Unicode block elements |
| TUI-WebGL | 2,100 | CRT post-processing (scanlines, phosphor glow, curvature), font atlas |
| Software Renderer | 620 | Pure software rasterization fallback |
| Tests | 7,500 | Widget state, focus management, cross-renderer validation |

**Architecture:** Render-agnostic C core with 3 swappable renderers. The pattern itself is the hard part.

### Shared Libraries — 65K LOC

**Estimated effort: 10-14 person-months**

43 headers, 53 source files covering:
- Geographic computing (Haversine, projections, tile math, bearing)
- Protocol buffers (varint, zigzag, delta, MVT encoding)
- HTTP server wrapper (Mongoose integration, request routing)
- Concurrency (thread-safe work queue, backpressure, timeout)
- JSON parser (full spec with error recovery)
- CSV parser (RFC4180 compliant)
- Rate limiting (token bucket, sliding window, per-IP)
- Circuit breaker, exponential backoff with jitter
- Spatial grid (2D indexing for geospatial queries)
- M/M/c queue capacity planning
- Font management (TrueType parsing, glyph metrics)
- PDF text extraction and layout reconstruction
- Hash maps (FNV-1a), binary heaps, priority queues, arena allocators
- 10 test files (~8,500 LOC)

### Locus Geocoder — 13K LOC

**Estimated effort: 3-4 person-months**

- OSM PBF parsing with node caching and tag classification
- Prefix trie for autocomplete (lazy allocation, 37-char alphabet)
- Trigram n-gram fuzzy matching (Jaccard similarity, FNV-1a hashing)
- Grid-based spatial index for reverse geocoding
- Memory-mapped binary serialization (compact packed structures)
- REST API + WASM + rate limiting

### FuelWise Optimizer — 12K LOC

**Estimated effort: 2-3 person-months** (assuming Ralph available)

- LP/MILP optimization via Ralph integration
- Two-step station filtering (overview → detailed polyline)
- Piecewise linear consumption modeling
- Point-to-segment distance with perpendicular snap
- Comprehensive validation suite (GLPK comparison)

### Surge VRP/PDPTW Solver — 8K LOC

**Estimated effort: 4-5 person-months**

- Adaptive Large Neighborhood Search (ALNS) with multiple destroy/repair operators
- Regret-based construction heuristic (k=3)
- Shaw removal, route/time/geographic clustering
- Stop-based state with doubly-linked list routes
- Multi-dimensional capacity constraints, time windows, pickup-delivery coupling
- 57 Solomon + 60 Li & Lim benchmark instances

### Site, Scripts, Documentation — 56K LOC

**Estimated effort: 4-6 person-months**

- API docs generator parsing C header `/*@api */` annotations
- Live WASM demos embedded in HTML (Velo, Carta, Ralph, Locus, FuelWise)
- CI/CD pipeline, service orchestration, data download
- 40K lines of documentation (architecture, manifesto, internals, 20+ roadmaps)

---

## Summary

| Category | LOC | Person-Months |
|----------|-----|---------------|
| Core Solvers (Ralph) | 81,337 | 24-36 |
| Routing (Velo) | 13,765 | 6-10 |
| Mapping (Carta) | 25,980 | 8-12 |
| UI Framework (ClayShards) | 23,187 | 8-12 |
| Infrastructure (Shared) | 64,602 | 10-14 |
| Geocoding (Locus) | 12,871 | 3-4 |
| Fuel Optimization (FuelWise) | 11,768 | 2-3 |
| VRP Solver (Surge) | 7,511 | 4-5 |
| Site, Scripts, Docs | 56,309 | 4-6 |
| **TOTAL** | **~297K** | **69-102** |

---

## Valuation Range

| Scenario | Person-Months | Cost |
|----------|---------------|------|
| **Low estimate** (experienced team, some reuse) | 69 | $1,000,000 - $1,400,000 |
| **Mid estimate** (realistic with iteration/debugging) | 85 | $1,300,000 - $1,700,000 |
| **High estimate** (matching quality, tests, docs) | 102 | $1,500,000 - $2,000,000 |

---

## Recommended Apport Value

**$1,500,000 - $1,750,000**

---

## Supporting Factors

1. **Specialized expertise required** — LP solver implementation requires numerical linear algebra expertise at graduate/PhD level. Sparse LU factorization, pivot stability, and MIP cutting planes are not general-purpose programming tasks.

2. **Zero external dependencies** — No GPL/LGPL libraries, clean IP, entire stack compiles to WASM. This is a concrete architectural differentiator: the demo IS the product.

3. **Production quality** — 700+ tests across all modules, GLPK comparison for Ralph, OSRM comparison for Velo, deterministic failure tracing, numerical stability hardening through iterative debugging.

4. **Integrated vertical stack** — Ralph solves LP/MIP. FuelWise formulates refueling as LP. Surge solves VRP. Velo routes. Carta renders tiles. Locus geocodes. ClayShards displays it all. Components designed to work together from solver to pixel.

5. **Transport-agnostic architecture** — Every module has pure C core + thin HTTP/WASM wrappers. A single HTML file can run the actual algorithms in-browser with zero server infrastructure.

6. **Team size estimate** — A team of 4-5 senior engineers working full-time would need approximately 18-24 months to reproduce the full stack, assuming expertise in LP solvers, graph algorithms, geospatial computing, and WebAssembly.

---

*Note: This valuation reflects reproduction cost only, not market value or revenue potential, which could be substantially higher with commercialization.*
