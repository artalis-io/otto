# Velo Routing Engine Roadmap

Comprehensive development plan for the Velo OSM routing engine covering algorithms, continental scale support, and planned enhancements.

## Status Summary (Feb 2026)

| Area | Status | Key Files |
|------|--------|-----------|
| **Dijkstra/A\*** | ✅ Complete | `vl_route.c` |
| **Bidirectional Search** | ✅ Complete | `vl_route.c` |
| **ALT (Landmarks)** | ✅ Complete | `vl_landmarks.c` |
| **Vehicle Profiles** | ✅ Complete | Car, Truck, Bike, Foot |
| **Binary Graph Format** | ✅ Complete | `.vlg` format |
| **Route Benchmark Tool** | ✅ Complete | `velo_route_bench.c` |
| **Turn-by-Turn Navigation** | ❌ TODO | Maneuver detection |
| **Distance/Duration Matrix** | ❌ TODO | Many-to-many routing |
| **Landmark Persistence** | ❌ TODO | Store landmarks in .vlg |
| **Continental Scale** | ❌ TODO | mmap, sparse restrictions |
| **Commercial Truck Routing** | ❌ TODO | Turn restrictions, dimensions, PTV DDS |
| **Contraction Hierarchies** | ❌ TODO (Phase 2) | CCH preprocessing |

**Current Performance**: ~100ms country-scale routes with ALT

---

## Chapter 1: Core Architecture

### 1.1 Data Flow

```
PBF File → vl_pbf.c → VLPBFContext → vl_graph.c → VLGraph → vl_route.c → VLRoute
```

### 1.2 Graph Format (CSR)

Compressed Sparse Row format for cache-friendly access:

```c
typedef struct {
    VLNode *nodes;        /* Array of nodes */
    VLEdge *edges;        /* Contiguous edge array */
    size_t num_nodes;
    size_t num_edges;
    VLLandmarks *landmarks;
} VLGraph;

typedef struct {
    double lat, lon;
    uint32_t first_edge;  /* Index into edge array */
    uint32_t edge_count;  /* Number of outgoing edges */
} VLNode;
```

### 1.3 Binary Graph Format (.vlg)

Pre-built graph files for fast loading (10-100x faster than PBF parsing):

```c
typedef struct {
    uint32_t magic;           /* 'VLG1' */
    uint32_t version;
    uint64_t num_nodes;
    uint64_t num_edges;
    uint64_t edge_offset;
    uint64_t node_offset;
    uint64_t landmark_offset;
    uint32_t flags;
} VLGraphHeader;
```

---

## Chapter 2: Routing Algorithms

### 2.1 Available Algorithms (✅ Complete)

| Algorithm | Use Case | Nodes Explored |
|-----------|----------|----------------|
| Dijkstra | Baseline shortest path | 100% of reachable |
| Bidirectional Dijkstra | Faster for long routes | ~50% |
| A* | Heuristic-guided | 30-50% of Dijkstra |
| Bidirectional A* | Best for long routes | ~25% |
| ALT (Landmarks) | Very long routes | 5-10% of A* |

### 2.2 Routing Modes

```c
typedef enum {
    VL_WEIGHT_DISTANCE,  /* Shortest path (meters) */
    VL_WEIGHT_DURATION   /* Fastest path (seconds) */
} VLWeightMode;
```

### 2.3 Vehicle Profiles

| Profile | Description | Avoids |
|---------|-------------|--------|
| `VL_PROFILE_CAR` | Standard car | Nothing |
| `VL_PROFILE_TRUCK` | HGV/truck | hgv=no, residential, service |
| `VL_PROFILE_BIKE` | Bicycle | Motorways, trunk roads |
| `VL_PROFILE_FOOT` | Pedestrian | Motorways, trunk, primary |

### 2.4 Landmarks (ALT Algorithm)

Precomputed shortest paths from strategic nodes for tighter A* bounds:

```c
VLLandmarks *lm = vl_landmarks_create(graph, 16);  /* 16 landmarks */
vl_route_astar_landmarks(graph, lm, source, target, &opts, &route);
```

**Dual-mode landmarks**: Both distance and time-based paths precomputed for optimal heuristics in either routing mode.

---

## Chapter 3: Turn-by-Turn Navigation (❌ TODO)

### 3.1 Goal

Generate human-readable driving instructions from route geometry.

### 3.2 Data Model

```c
typedef struct {
    double lat, lon;           /* Maneuver location */
    double distance_m;         /* Distance from previous */
    double duration_s;         /* Time from previous */
    VLManeuverType type;       /* TURN_LEFT, TURN_RIGHT, CONTINUE, etc. */
    int exit_number;           /* For roundabouts/exits */
    char road_name[128];       /* Road to turn onto */
    char instruction[256];     /* Human-readable instruction */
} VLManeuver;

typedef struct {
    VLManeuver *maneuvers;
    size_t maneuver_count;
    double total_distance_m;
    double total_duration_s;
} VLTurnByTurn;
```

### 3.3 Features

- Road name extraction from OSM way tags
- Maneuver detection (turn, continue, merge, exit)
- Distance-to-next-maneuver
- Voice instruction text generation
- Multi-language support (i18n)

### 3.4 API

```c
int vl_route_turn_by_turn(VLGraph *graph, VLRoute *route, VLTurnByTurn *out);
void vl_turn_by_turn_free(VLTurnByTurn *tbt);
```

---

## Chapter 4: Distance/Duration Matrix (❌ TODO)

### 4.1 Goal

Compute many-to-many distance and duration matrices efficiently.

### 4.2 Use Cases

- Fleet dispatch (assign N drivers to M loads)
- Clustering for route optimization
- Service area analysis

### 4.3 API

```c
typedef struct {
    double *distances;         /* [sources * targets] matrix */
    double *durations;         /* [sources * targets] matrix */
    size_t num_sources;
    size_t num_targets;
} VLMatrix;

int vl_compute_matrix(
    VLGraph *graph,
    const VLCoord *sources, size_t num_sources,
    const VLCoord *targets, size_t num_targets,
    VLRouteOptions *opts,
    VLMatrix *out
);
```

### 4.4 Optimization Strategies

1. **Single-source multi-target Dijkstra**: Compute all targets in one search
2. **Early termination**: Stop when all targets found
3. **Parallel computation**: Parallelize across sources
4. **Contraction Hierarchies**: O(1) lookups after preprocessing (Phase 2)

---

## Chapter 5: ALT Speedup (❌ TODO)

### 5.1 Problem

Landmarks computed at server startup add 4-8 seconds for country-scale graphs.

### 5.2 Current State

- 32 landmarks × 4 Dijkstra runs = 128 Dijkstra runs at startup
- Landmarks NOT stored in `.vlg` file
- Must recompute on every server restart

### 5.3 Solution: Persist Landmarks to .vlg

| Option | Startup | Index Size | Status |
|--------|---------|------------|--------|
| Compute at startup | 4-8s | Unchanged | Current |
| **Persist to .vlg** | <1s | +50-100MB | Recommended |
| Separate file | <1s | Separate | Alternative |
| Lazy computation | ~0s | Unchanged | Alternative |

### 5.4 Implementation

```c
/* Extend vl_graph_save() */
write_u32(num_landmarks);
write_u32(num_nodes);
for (int l = 0; l < num_landmarks; l++) {
    write_u32(landmark_node_id[l]);
    write_doubles(dist_to_landmark[l], num_nodes);
    write_doubles(dist_from_landmark[l], num_nodes);
    write_doubles(time_to_landmark[l], num_nodes);
    write_doubles(time_from_landmark[l], num_nodes);
}
```

---

## Chapter 6: Continental Scale Support (❌ TODO)

### 6.1 Requirements

| Requirement | Value |
|-------------|-------|
| Query latency | ~2 seconds (95th percentile) |
| Memory budget | ~120GB total (separate US/EU deployments) |
| Target regions | US (~36GB), EU+UK (~72GB) |
| Truck constraints | Weight, height, length, axle limits, hazmat |

### 6.2 Scale Requirements

| Region | PBF Size | Nodes | Edges | RAM Needed |
|--------|----------|-------|-------|------------|
| Country (Hungary) | 300MB | ~3M | ~5M | ~4GB |
| US (contiguous) | ~10GB | ~175M | ~250M | ~36GB |
| EU + UK | ~25GB | ~350M | ~500M | ~72GB |

### 6.3 Memory-Mapped Graph Storage

Load large graphs without 2x memory spike during parsing:

```c
typedef enum {
    VL_STORAGE_MALLOC,  /* Traditional: parse PBF → malloc */
    VL_STORAGE_MMAP     /* Memory-mapped: load pre-built .vlg */
} VLStorageMode;

VLGraph *vl_graph_mmap(const char *path);
int vl_graph_save(VLGraph *g, const char *path);
void vl_graph_unmap(VLGraph *g);
```

### 6.4 Sparse Truck Restriction Storage

Store truck constraints only for edges that have restrictions (~1-5% of edges):

```c
typedef struct {
    uint16_t flags;               /* VL_RESTRICT_* flags */
    uint16_t max_weight_100kg;    /* Max weight in 100kg units */
    uint8_t max_height_dm;        /* Max height in decimeters */
    uint8_t max_length_dm;        /* Max length in decimeters */
    uint8_t max_width_dm;         /* Max width in decimeters */
    uint8_t max_axle_weight_100kg;
    uint8_t max_axle_count;
    uint8_t hazmat_flags;
} VLTruckRestriction;

/* Sparse index: O(log n) binary search */
typedef struct {
    uint64_t *edge_ids;           /* Sorted edge IDs */
    uint32_t *offsets;            /* Offset into data */
    VLTruckRestriction *data;
    size_t count;
} VLRestrictionIndex;
```

**Memory estimate**: 12.5M restricted edges × 24 bytes = 300MB for US.

### 6.5 Turn Restriction Support

```c
typedef struct {
    uint64_t from_edge;
    uint32_t via_node;
    uint64_t to_edge;
    uint16_t restriction_type;    /* VL_TURN_NO_LEFT, etc. */
    uint16_t vehicle_flags;       /* Which vehicles affected */
} VLTurnRestriction;
```

### 6.6 ALT Optimization for Continental Scale

Reduce landmark count with strategic placement:

**Current**: 16 landmarks
**Target**: 8-12 landmarks with compression

```c
/* Compress distances to 2 bytes with log-scale */
uint16_t vl_compress_distance(double dist_meters) {
    if (dist_meters <= 0) return 0;
    if (dist_meters >= 5000000) return UINT16_MAX;
    return (uint16_t)(log(dist_meters + 1) * 4000);
}
```

**Memory with compression**:
- US: 175M × 4 bytes × 12 = 8.4GB (vs 16.8GB uncompressed)
- EU: 350M × 4 bytes × 12 = 16.8GB (vs 33.6GB uncompressed)

### 6.7 Graph Builder CLI

```
Usage: vl-build [options] <input.osm.pbf> <output.vlg>

Options:
  -p, --progress        Show progress bar
  -c, --checkpoint DIR  Enable checkpoint/recovery
  -m, --memory LIMIT    Memory limit (e.g., 64G)
  -j, --jobs N          Parallel jobs
  -l, --landmarks N     Number of ALT landmarks (default: 12)
  --profile PROFILE     Vehicle profile
  --no-restrictions     Skip truck restriction parsing
```

### 6.8 Data Structures for Phase 2 CCH

Design graph structure that supports later CCH overlay:

```c
typedef struct {
    double lat, lon;
    uint32_t first_edge;
    uint32_t edge_count;
    uint32_t cch_level;       /* 0 = unordered, >0 = CCH level */
    uint32_t cch_first_up;    /* First upward CCH edge */
    uint32_t cch_first_down;  /* First downward CCH edge */
} VLNode;

typedef struct {
    uint32_t target;
    uint32_t cost;
    uint64_t osm_way_id;      /* 0 for shortcuts */
    uint32_t middle_node;     /* For shortcuts: skipped node */
} VLEdge;
```

---

## Chapter 7: Contraction Hierarchies (Phase 2 - Future)

### 7.1 Goal

Sub-100ms queries via preprocessed hierarchy.

### 7.2 CCH Benefits

- Pre-compute node ordering and shortcuts once (expensive, ~hours)
- Customize edge weights quickly (~minutes for new truck profile)
- Query in milliseconds

### 7.3 CCH API (Planned)

```c
/* Build CCH overlay (expensive, do once per region) */
int vl_cch_build(VLGraph *g, const VLCCHConfig *config);

/* Customize for specific vehicle profile (fast) */
int vl_cch_customize(VLGraph *g, const VLVehicleProfile *profile);

/* Query using CCH (fast) */
int vl_route_cch(VLGraph *g, uint32_t from, uint32_t to,
                 const VLVehicleProfile *profile, VLRoute *out);
```

---

## Chapter 8: Testing & Benchmarks

### 8.1 Current Test Coverage

- 51 velo unit tests
- Geo, heap, protobuf, graph, routing, vehicle profiles

### 8.2 Benchmarks

```bash
# Run routing benchmarks
./bench_routing

# Parse and benchmark a PBF file
./bench_pbf map.osm.pbf output.vlg
```

### 8.3 Route Quality Benchmark Tool (✅ Complete)

Automated feedback loop for routing quality and performance optimization.

**Usage:**
```bash
make route-bench
./velo-route-bench --graph data/hungary.vlg --suite smoke -v
./velo-route-bench --graph data/hungary.vlg --compare-algorithms routes/*.json
```

**Implemented Features:**
- JSON-based route definitions with origin/destination, profile, tolerances
- Distance validation (expected vs actual with tolerance)
- Duration validation
- Profile compliance checking (VL_ACCESS_NO_TRUCK, etc.)
- Algorithm consistency (all algorithms return same optimal distance)
- Performance metrics (query_time_ms, nodes_explored, ms_per_km)
- OSRM comparison (--osrm-url) - compare against reference implementation
- Structured JSON output for CI integration
- Skill: `/velo-route-bench` for debugging workflow

**Benchmark Routes (4 created, ~25 planned):**

| Category | Routes | Status |
|----------|--------|--------|
| Urban | budapest_downtown.json | ✅ |
| Regional | budapest_szeged.json | ✅ |
| Cross-country | sopron_nyiregyhaza.json | ✅ |
| Profiles | truck_avoid_residential.json | ✅ |
| Edge cases | ferry, tunnel, one-way maze | ❌ TODO |

**TODO:**
- [ ] Create remaining ~20 benchmark routes
- [ ] Route assertions (avoids_highway_class, passes_near)
- [ ] CI integration (GitHub Actions)
- [ ] Baseline performance capture

**Key Files:**
- `benchmarks/velo_route_bench.c` - CLI tool
- `benchmarks/routes/` - Benchmark route definitions
- `.claude/skills/velo-route-bench/skill.md` - Debugging workflow

### 8.4 Performance Notes

| Metric | Value |
|--------|-------|
| Binary graph loading | 10-100x faster than PBF |
| A* vs Dijkstra | 2-3x fewer nodes explored |
| Bidirectional | Halves search space |
| ALT speedup | 5-10x for long routes |

---

## Chapter 9: Implementation Roadmap

### Completed

1. ✅ Dijkstra, A*, Bidirectional algorithms
2. ✅ ALT (Landmarks) algorithm
3. ✅ Vehicle profiles (car, truck, bike, foot)
4. ✅ Binary graph format (.vlg)
5. ✅ PBF parsing

### Next Up (Feature Enhancements)

| Task | Priority | Effort |
|------|----------|--------|
| Persist landmarks to .vlg | P1 | 1 week |
| Turn-by-turn navigation | P2 | 2 weeks |
| Distance/duration matrix | P2 | 2 weeks |

### Continental Scale (Phase 1)

| Milestone | Weeks | Description |
|-----------|-------|-------------|
| Core Infrastructure | 1-2 | mmap abstraction, graph file format |
| Truck Restrictions | 3-4 | Sparse index, turn restrictions |
| Preprocessing | 5-6 | Build CLI, progress, checkpoints |
| Query Optimization | 7-8 | Distance compression, ALT tuning |
| Integration | 9-10 | API server, testing |

### CCH (Phase 2 - Future)

- Node ordering algorithms
- Shortcut computation
- Customization for truck profiles
- Sub-100ms query routing

---

## Chapter 10: Memory Budget

### US Deployment (~36GB)

| Component | Size |
|-----------|------|
| Nodes (175M) | 4.2GB |
| Edges (250M) | 4.0GB |
| Restriction index | 0.3GB |
| Turn restrictions | 0.1GB |
| Landmarks (12, compressed) | 8.4GB |
| Working memory | 2-4GB |
| **Total** | **~20GB** |

### EU+UK Deployment (~72GB)

| Component | Size |
|-----------|------|
| Nodes (350M) | 8.4GB |
| Edges (500M) | 8.0GB |
| Restriction index | 0.6GB |
| Turn restrictions | 0.2GB |
| Landmarks (12, compressed) | 16.8GB |
| Working memory | 2-4GB |
| **Total** | **~37GB** |

---

## File Structure

```
velo/
├── include/
│   ├── velo.h            # Unified API
│   ├── vl_types.h        # Data structures
│   ├── vl_pbf.h          # PBF parsing
│   ├── vl_graph.h        # Graph operations
│   ├── vl_route.h        # Routing algorithms
│   └── vl_landmarks.h    # ALT algorithm
├── src/
│   ├── vl_route.c        # Dijkstra, A*, bidirectional
│   ├── vl_landmarks.c    # ALT implementation
│   ├── vl_graph.c        # CSR graph construction
│   ├── vl_pbf.c          # PBF parsing
│   ├── vl_heap.c         # Priority queue
│   └── vl_geo.c          # Haversine, coordinates
├── api/                  # Route server REST API
├── tools/                # CLI tools (vl-build)
├── tests/                # Test suite
└── benchmarks/
    ├── bench_routing.c       # Algorithm benchmarks
    ├── bench_pbf.c           # PBF parsing benchmarks
    ├── velo_route_bench.c    # Route quality benchmark
    └── routes/               # Benchmark route definitions
        ├── urban/
        ├── regional/
        ├── cross_country/
        └── profiles/
```

---

## Dependencies

| Library | Location | Purpose |
|---------|----------|---------|
| Shared | `../shared/` | Protobuf, inflate, geo utilities |
| miniz | `../vendor/miniz/` | zlib compression (via shared) |

---

## Success Criteria

### Phase 1 (Continental)

- [ ] US continental query in <2 seconds (P95)
- [ ] EU continental query in <2 seconds (P95)
- [ ] Truck restrictions correctly applied
- [ ] Memory under 40GB for US, 80GB for EU
- [ ] Graph build completes without OOM

### Feature Enhancements

- [ ] Turn-by-turn instructions generated
- [ ] Distance matrix computed efficiently
- [ ] Landmarks persisted to .vlg (instant startup)

---

## Chapter 11: Performance Optimization Strategies

### 11.1 Current Benchmarks vs OSRM

| Metric | OSRM | Velo (Current) | Gap |
|--------|------|----------------|-----|
| Query Time | < 1ms | 20-500ms | 20-500x |
| Preprocessing | Hours | Seconds | OSRM slower |
| Memory (Graph) | 2-8 GB | 50-300 MB | Velo better |
| Algorithm | Contraction Hierarchies | A* / Dijkstra | Fundamental |

**Key Insight**: OSRM trades preprocessing time (hours) and memory (GB) for sub-millisecond queries. Velo uses no preprocessing but pays at query time.

### 11.2 Current Performance (Hungary - 2.7M nodes)

| Algorithm | Time | Notes |
|-----------|------|-------|
| Baseline A* bidirectional | 130-285ms | No preprocessing |
| With ALT (16 landmarks) | 36-86ms | 3-4x speedup |
| Target | <10ms | Approaching OSRM |

### 11.3 Recommended Optimizations (Without CCH)

| Priority | Technique | Expected Speedup | Effort |
|----------|-----------|------------------|--------|
| **P1** | Arc Flags | 3-5x → ~15ms | 2-3 days |
| **P2** | Reach Pruning | 2-3x additional | 1-2 days |
| **P3** | Goal-Directed Arc Flags | 10-20x combined → ~5ms | 1 week |
| **P4** | Hub Labeling | Sub-millisecond | 2+ weeks |

### 11.4 Arc Flags (Best ROI)

Partition graph into 64-128 regions and precompute reachability flags per edge:

```c
// During search: single bitmask AND prunes 70-90% of edges
if (!(edge->arc_flags & (1ULL << target_region))) continue;
```

**Preprocessing**: ~30 minutes (parallelizable)
**Memory**: +8 bytes/edge (64 regions) = ~44MB for Hungary

### 11.5 Known Bottlenecks

| Issue | Severity | Impact |
|-------|----------|--------|
| No reverse graph index | CRITICAL | O(V×E) per backward expansion |
| Full array initialization | HIGH | O(V) even for short routes |
| Haversine per edge | MEDIUM | Expensive trig on every relaxation |
| No spatial index for nearest | MEDIUM | O(V) linear scan |

### 11.6 Quick Wins

1. **Reverse adjacency list**: Store incoming edges for O(degree) backward expansion
2. **Lazy initialization**: Use sentinel values + visited set instead of full init
3. **Haversine LUT**: Precompute or approximate with Euclidean for A* heuristic
4. **K-d tree for nearest**: O(log V) nearest node lookup

---

## Chapter 12: Commercial Truck Routing (❌ TODO)

> **Detailed Plan:** See `velo-commercial-routing.md` for full analysis

### 12.1 Overview

Commercial-grade truck routing requires features beyond basic OSM access flags:

| Feature | Current State | Required |
|---------|---------------|----------|
| Turn restrictions | ❌ None | Edge-to-edge prohibitions |
| Dimensional constraints | ❌ None | Height, weight, width, length, axle load |
| Hazmat routing | ❌ None | Tunnel codes, hazmat categories |
| Commercial data formats | ❌ OSM only | PTV DDS, TomTom GDF-AS |
| Time-dependent routing | ❌ None | Conditional restrictions |

### 12.2 Proposed Data Structures

```c
/* Sparse restriction record (only for edges with limits) */
typedef struct {
    uint16_t max_height_dm;       /* Decimeters (0 = no limit) */
    uint16_t max_width_dm;
    uint32_t max_weight_kg;
    uint16_t max_length_dm;
    uint16_t max_axle_kg;         /* In 100kg units */
    uint8_t hazmat_flags;
    uint8_t tunnel_code;          /* ADR A-E */
} VLEdgeRestriction;

/* Turn restriction */
typedef struct {
    uint32_t from_edge;
    uint32_t to_edge;
    uint8_t restriction_type;     /* PROHIBITED, REQUIRED */
    uint8_t vehicle_mask;
} VLTurnRestriction;

/* Full truck profile */
typedef struct {
    uint16_t height_dm, width_dm, length_dm;
    uint32_t gross_weight_kg;
    uint16_t axle_weight_100kg;
    uint8_t hazmat_class;
    uint8_t tunnel_category;
} VLTruckProfile;
```

### 12.3 Commercial Data Formats

| Format | File Types | Parser Effort | Recommendation |
|--------|------------|---------------|----------------|
| **PTV DDS** | Shapefile, MIF/MID, TAB | 2-3 weeks | **Start here** |
| TomTom GDF-AS | CSV/TSV | 3-4 weeks | Add later if needed |

**PTV DDS advantages:**
- Simpler format (Shapefile is well-documented)
- Lower licensing cost
- Sample data available for development
- Strong European logistics adoption (Girteka, Waberer's)
- Independent of Trimble (supports vendor lock-in narrative)

### 12.4 Implementation Timeline (~9 weeks)

| Phase | Duration | Deliverable |
|-------|----------|-------------|
| 0: Schema | 1 week | Extended data structures |
| 1: Turn restrictions | 2 weeks | Turn enforcement in all algorithms |
| 2: Dimensional constraints | 1 week | Height/weight/width filtering |
| 3: PTV DDS parser | 2-3 weeks | Commercial data ingestion |
| 4: Integration | 2 weeks | End-to-end testing |

### 12.5 API Changes

```
GET /api/v1/route?from=...&to=...&profile=truck
    &height=4.0       # Vehicle height in meters
    &width=2.55       # Vehicle width in meters
    &weight=40000     # Gross weight in kg
    &length=16.5      # Vehicle length in meters
    &hazmat=3         # UN hazmat class (optional)
```

### 12.6 Success Criteria

- [ ] Turn restrictions enforced (no illegal turns)
- [ ] Dimensional constraints checked (no low bridges)
- [ ] PTV DDS data successfully ingested
- [ ] Routes validated against known truck GPS traces
- [ ] API accepts truck dimensions

## Keel Migration — Phase 4 of 6 (✅ Complete)

**Completed for Velo.** `velo/api` runs on Keel v3.

Rationale and shared context: `docs/roadmaps/surge.md` (Phase 1). The previous
server was `GPL-2.0-only or commercial`, incompatible with OTTO's AGPL-3.0 and
unsublicensable for the commercial tier; Keel is MIT.

### The largest port so far

Velo had 100 legacy HTTP call sites — and unlike Surge, Ralph and FuelWise the
old server was used for *parsing*, not just transport:

| Previous behavior | Keel replacement |
|-------------------|------------------|
| `mg_http_var(query, "k")` | `sh_query_get_str()` (`shared/include/sh_query.h`) |
| `mg_json_get_str(body, "$.k")` | `sh_json_parse()` + `sh_json_get_path()` + `sh_json_as_string()` |
| `mg_json_get_bool(body, "$.k", &b)` | `sh_json_as_bool()` |
| `mg_match(str, mg_str("x"), NULL)` | `strcmp()` |
| `struct mg_str` params | `const char *` |

All replacements are existing OTTO shared code, so nothing new was written for
this and the parsing became transport-agnostic in the process. The JSON body is
parsed into a scratch `SHArena` that is released before the handler returns.

### Async

`ShWorkQueue` + `ShWorkerPool` + `ShCompletion` give way to `KlThreadPool` +
`KlAsyncOp`, matching Surge and FuelWise, with the same `RouteCtx` ownership
rules and the `on_resume` fix (see `docs/roadmaps/fuelwise.md` for why that is
required). The worker now does route + polyline encode + JSON render and hands
back a finished response string, so the event loop is free for the whole solve
rather than blocking in `sh_completion_wait()`.

`/api/v1/stats` keeps its `work_queue` shape; `KlThreadPool` exposes no
statistics, so `VeloQueueStats` tracks pushed/popped/dropped/expired, all
touched only on the event loop thread.

### Verification

Velo needs an OSM graph to start, which is why CI was build + `--help` only.
That is fixed: `make data/monaco.vlg` already knew how to fetch Monaco (~700KB)
and build the index, so `velo/api/test_api.sh` now starts the server against it
and exercises the endpoints, gated in CI (exits non-zero on failure).

Verified by hand against `data/monaco.vlg` (7,334 nodes / 11,826 edges):

| Check | Result |
|---|---|
| `GET /route` query string | `"status":"ok"`, 2.4km / 170s, polyline returned |
| `POST /route` JSON body | truck + `mode:shortest` honoured, 2.3km / 190s |
| `geometry=false` / `"geometry":false` | geometry key omitted (both GET and POST) |
| Missing / invalid / out-of-bounds coords | 400 with the expected messages |
| Malformed JSON body | `{"error":"Invalid JSON body"}` |
| Unknown path / wrong method / preflight | 404 / 405 / 204 |
| Async dispatch | `pushed`/`popped` counters advance; 5 concurrent routes ~1ms each, health 0.78ms during |

GET and POST were cross-checked on the same inputs: `foot`+`shortest` returns
"No route found" on both, confirming the JSON path and the query path agree
rather than one silently mis-parsing.

### Status: Complete

All six servers are on Keel v3; the legacy `shared/src/sh_httpserver.c` has
been removed. See `docs/roadmaps/infrastructure.md` for the cross-cutting
completion record.
