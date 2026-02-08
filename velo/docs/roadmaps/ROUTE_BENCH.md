# Velo Route Benchmark Roadmap

## Overview

A comprehensive benchmarking and debugging tool for Velo routing, providing automated feedback loops for quality and performance optimization. Similar to Ralph's NETLIB benchmark system but tailored for routing problems.

**Goal:** Achieve routing quality and performance parity with commercial routers (OSRM, GraphHopper) while maintaining zero external dependencies.

## Problem Statement

Currently, Velo has:
- Basic benchmarks (`bench_routing.c`) that test algorithms on synthetic grids
- Real-world routing tests but no systematic quality validation
- No automated way to detect regressions in route quality or performance
- No comparison against reference implementations

We need:
- Automated validation of route correctness (distance, duration, profile compliance)
- Performance regression detection
- Systematic debugging workflow for routing issues
- CI-friendly benchmark suite

## Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                        velo-route-bench CLI                         │
├─────────────────────────────────────────────────────────────────────┤
│  Route Loader    │  Validator   │  Comparator  │  JSON Reporter    │
│  (JSON files)    │  (quality)   │  (algorithms)│  (structured out) │
├─────────────────────────────────────────────────────────────────────┤
│                         Velo Library (libvelo.a)                    │
│  vl_route.c │ vl_graph.c │ vl_landmarks.c │ vl_heap.c │ vl_pbf.c   │
└─────────────────────────────────────────────────────────────────────┘
```

### File Structure

```
velo/
├── benchmarks/
│   ├── bench_routing.c          # Existing algorithm benchmarks
│   ├── bench_pbf.c              # Existing PBF parsing benchmarks
│   ├── velo_route_bench.c       # NEW: Route quality benchmark tool
│   └── routes/                  # NEW: Benchmark route definitions
│       ├── urban/
│       │   ├── budapest_downtown.json
│       │   ├── vienna_city.json
│       │   └── munich_center.json
│       ├── regional/
│       │   ├── budapest_szeged.json
│       │   ├── budapest_gyor.json
│       │   ├── vienna_salzburg.json
│       │   └── munich_frankfurt.json
│       ├── cross_country/
│       │   ├── sopron_nyiregyhaza.json
│       │   ├── pecs_debrecen.json
│       │   └── vienna_budapest.json
│       ├── edge_cases/
│       │   ├── ferry_crossing.json
│       │   ├── tunnel_restriction.json
│       │   ├── one_way_maze.json
│       │   ├── no_left_turn.json
│       │   └── construction_detour.json
│       └── profiles/
│           ├── truck_avoid_residential.json
│           ├── truck_weight_limit.json
│           ├── bike_avoid_motorway.json
│           └── foot_prefer_paths.json
├── docs/
│   └── roadmaps/
│       └── ROUTE_BENCH.md       # This document
└── .claude/
    └── skills/
        └── velo-route-bench/
            └── skill.md         # NEW: Claude skill for debugging
```

## Tool Specification

### 1. Benchmark Route Format

Each route is defined in a JSON file:

```json
{
  "name": "Budapest → Szeged",
  "description": "Regional route through Hungarian plains",
  "category": "regional",

  "origin": {
    "lat": 47.4979,
    "lon": 19.0402,
    "name": "Budapest Keleti"
  },
  "destination": {
    "lat": 46.2530,
    "lon": 20.1414,
    "name": "Szeged Center"
  },

  "options": {
    "profile": "car",
    "weight": "distance",
    "algorithm": "astar_bidir"
  },

  "reference": {
    "source": "dijkstra_exhaustive",
    "computed_at": "2026-02-08",
    "graph_version": "hungary-250208",
    "distance_m": 174832,
    "duration_s": 7920,
    "waypoint_count": 1247,
    "checksum": "sha256:abc123..."
  },

  "tolerances": {
    "distance_pct": 0.1,
    "duration_pct": 1.0,
    "time_ms_max": 100
  },

  "assertions": [
    {"type": "avoids_highway_class", "class": "residential"},
    {"type": "uses_highway_class", "class": "motorway"},
    {"type": "passes_near", "lat": 46.9, "lon": 19.7, "radius_km": 5}
  ]
}
```

### 2. Validation Types

#### 2.1 Distance Validation

```c
typedef struct {
    double expected_m;        // Reference distance
    double actual_m;          // Velo result
    double error_m;           // Absolute error
    double error_pct;         // Percentage error
    int within_tolerance;     // Pass/fail
} DistanceValidation;
```

**Tolerance levels:**
| Category | Default Tolerance | Rationale |
|----------|-------------------|-----------|
| Urban (<10km) | 0.5% | Short routes need precision |
| Regional (10-200km) | 0.1% | Main use case, strict |
| Cross-country (>200km) | 0.05% | Long routes, very strict |
| Edge cases | 1.0% | May have valid alternatives |

#### 2.2 Duration Validation

```c
typedef struct {
    double expected_s;        // Reference duration
    double actual_s;          // Velo result
    double error_s;           // Absolute error
    double error_pct;         // Percentage error
    int within_tolerance;     // Pass/fail
} DurationValidation;
```

**Duration is harder to validate** because:
- Speed assumptions vary (Velo uses conservative speeds)
- Traffic data not included
- Road class speed mapping differs between routers

**Approach:** Allow 5-10% duration tolerance, focus on distance accuracy.

#### 2.3 Profile Compliance Validation

```c
typedef struct {
    VLProfile profile;           // Expected profile
    int edges_checked;           // Total edges in route
    int violations_found;        // Edges that violate profile
    ViolationDetail *violations; // Details of each violation
    int compliant;               // Overall pass/fail
} ProfileValidation;

typedef struct {
    uint32_t edge_idx;           // Edge index in graph
    uint64_t way_id;             // OSM way ID
    const char *highway_class;   // e.g., "residential"
    const char *restriction;     // e.g., "hgv=no"
    const char *reason;          // Why this is a violation
} ViolationDetail;
```

**Profile rules to check:**

| Profile | Must Avoid | Should Prefer |
|---------|------------|---------------|
| `truck` | hgv=no, residential, service, maxweight violations | trunk, primary |
| `bike` | motorway, trunk | cycleway, path, residential |
| `foot` | motorway, trunk, primary | footway, path, pedestrian |
| `car` | access=no | (none) |

#### 2.4 Algorithm Consistency Validation

```c
typedef struct {
    VLAlgorithm algorithms[6];   // All tested algorithms
    double distances[6];         // Distance per algorithm
    double times_ms[6];          // Query time per algorithm
    uint32_t nodes_explored[6];  // Nodes per algorithm
    int all_consistent;          // All return same distance?
    double max_distance_diff;    // Max diff between algorithms
} ConsistencyValidation;
```

**Consistency rules:**
- All algorithms must return the same optimal distance
- A* must explore fewer nodes than Dijkstra
- Bidirectional must explore fewer nodes than unidirectional
- ALT (landmarks) must explore fewer nodes than plain A*

### 3. Performance Metrics

```c
typedef struct {
    // Timing
    double query_time_ms;        // Total query time
    double graph_load_time_ms;   // Time to load graph (if applicable)
    double landmark_time_ms;     // Landmark preprocessing (if applicable)

    // Search efficiency
    uint32_t nodes_explored;     // Nodes popped from heap
    uint32_t edges_relaxed;      // Edges examined
    uint32_t heap_operations;    // Push + pop + decrease-key

    // Memory
    size_t peak_memory_bytes;    // Peak RSS during query
    size_t graph_memory_bytes;   // Graph structure size
    size_t landmark_memory_bytes;// Landmark data size

    // Derived metrics
    double ms_per_km;            // Time per km of route
    double nodes_per_km;         // Nodes explored per km
    double speedup_vs_dijkstra;  // A*/ALT speedup factor
} PerformanceMetrics;
```

**Performance targets:**

| Metric | Target | Acceptable | Needs Work |
|--------|--------|------------|------------|
| Urban query | <10ms | <50ms | >100ms |
| Regional query | <50ms | <100ms | >200ms |
| Cross-country query | <100ms | <200ms | >500ms |
| Nodes/km (A* bidir) | <200 | <500 | >1000 |
| Speedup vs Dijkstra | >3x | >2x | <1.5x |

### 4. JSON Output Format

Complete output for a single route benchmark:

```json
{
  "benchmark": {
    "tool_version": "1.0.0",
    "timestamp": "2026-02-08T14:30:00Z",
    "graph_file": "hungary-latest.vlg",
    "graph_nodes": 2734521,
    "graph_edges": 5891234
  },

  "route": {
    "name": "Budapest → Szeged",
    "file": "routes/regional/budapest_szeged.json",
    "category": "regional",
    "origin": {"lat": 47.4979, "lon": 19.0402},
    "destination": {"lat": 46.2530, "lon": 20.1414},
    "profile": "car",
    "weight": "distance"
  },

  "reference": {
    "source": "dijkstra_exhaustive",
    "distance_m": 174832,
    "duration_s": 7920,
    "waypoints": 1247
  },

  "result": {
    "status": "ok",
    "algorithm": "astar_bidir",
    "distance_m": 174832,
    "duration_s": 7915,
    "waypoints": 1243,
    "geometry_points": 8934
  },

  "validation": {
    "overall": "pass",

    "distance": {
      "pass": true,
      "expected_m": 174832,
      "actual_m": 174832,
      "error_m": 0,
      "error_pct": 0.0,
      "tolerance_pct": 0.1
    },

    "duration": {
      "pass": true,
      "expected_s": 7920,
      "actual_s": 7915,
      "error_s": 5,
      "error_pct": 0.06,
      "tolerance_pct": 5.0
    },

    "profile": {
      "pass": true,
      "edges_checked": 1243,
      "violations": 0,
      "violation_details": []
    },

    "consistency": {
      "pass": true,
      "algorithms_tested": ["dijkstra", "dijkstra_bidir", "astar", "astar_bidir"],
      "all_same_distance": true,
      "distances": [174832, 174832, 174832, 174832]
    },

    "assertions": {
      "pass": true,
      "results": [
        {"assertion": "avoids_highway_class:residential", "pass": true},
        {"assertion": "uses_highway_class:motorway", "pass": true}
      ]
    }
  },

  "performance": {
    "query_time_ms": 23.4,
    "nodes_explored": 42315,
    "edges_relaxed": 89234,
    "heap_operations": 84630,

    "derived": {
      "ms_per_km": 0.134,
      "nodes_per_km": 242,
      "speedup_vs_dijkstra": 3.2
    },

    "by_algorithm": {
      "dijkstra": {"time_ms": 74.2, "nodes": 134521},
      "dijkstra_bidir": {"time_ms": 38.1, "nodes": 71234},
      "astar": {"time_ms": 31.2, "nodes": 52341},
      "astar_bidir": {"time_ms": 23.4, "nodes": 42315}
    }
  },

  "diagnosis": {
    "issues": [],
    "warnings": [],
    "recommendations": []
  }
}
```

### 5. CLI Interface

```bash
velo-route-bench [OPTIONS] [ROUTE_FILES...]

ARGUMENTS:
  ROUTE_FILES              One or more route JSON files to benchmark

OPTIONS:
  -g, --graph FILE         Graph file (.vlg or .osm.pbf) [required]
  -o, --output FILE        Output JSON file (default: stdout)
  -v, --verbose            Print progress to stderr
  -q, --quiet              Suppress all output except errors

ROUTE SELECTION:
  --category CAT           Run all routes in category (urban/regional/cross_country/edge_cases/profiles)
  --suite SUITE            Run predefined suite (smoke/quick/full/stress)

ALGORITHM OPTIONS:
  --algorithm ALG          Test specific algorithm (dijkstra/dijkstra_bidir/astar/astar_bidir/alt)
  --compare-algorithms     Run all algorithms and compare
  --landmarks N            Use N landmarks for ALT (default: 16)
  --no-landmarks           Disable landmark preprocessing

PROFILE OPTIONS:
  --profile PROFILE        Override route profile (car/truck/bike/foot)
  --compare-profiles       Run all profiles and compare

VALIDATION OPTIONS:
  --distance-tolerance PCT Distance error tolerance (default: 0.1%)
  --duration-tolerance PCT Duration error tolerance (default: 5.0%)
  --strict                 Fail on any warning
  --permissive             Only fail on errors, ignore warnings

REFERENCE OPTIONS:
  --generate-reference     Compute reference using exhaustive Dijkstra
  --update-reference       Update reference in route files
  --osrm-url URL           Compare against OSRM server

PERFORMANCE OPTIONS:
  --iterations N           Run each route N times (default: 1)
  --warmup N               Warmup iterations before timing (default: 0)
  --time-limit MS          Max time per route (default: 10000)

OUTPUT OPTIONS:
  --format FMT             Output format: json/table/csv (default: json)
  --summary-only           Only output summary, not per-route details

UTILITY:
  --list-routes            List available benchmark routes
  --validate-routes        Check route files are valid JSON
  --help                   Show this help
  --version                Show version
```

### 6. Test Suites

| Suite | Routes | Purpose | Time |
|-------|--------|---------|------|
| `smoke` | 3 | Quick sanity check | ~1s |
| `quick` | 10 | Fast CI validation | ~5s |
| `full` | 30+ | Complete validation | ~30s |
| `stress` | 50+ | Performance regression | ~2min |

Suite definitions:

```c
typedef struct {
    const char *name;
    const char *description;
    const char **route_patterns;  // Glob patterns
    int num_patterns;
} BenchSuite;

static const BenchSuite SUITES[] = {
    {
        "smoke",
        "Quick sanity check (3 routes)",
        (const char *[]){"routes/regional/budapest_szeged.json",
                         "routes/urban/budapest_downtown.json",
                         "routes/profiles/truck_avoid_residential.json"},
        3
    },
    {
        "quick",
        "Fast CI validation (10 routes)",
        (const char *[]){"routes/regional/*.json",
                         "routes/urban/budapest_*.json"},
        2
    },
    {
        "full",
        "Complete validation (all routes)",
        (const char *[]){"routes/**/*.json"},
        1
    },
    {
        "stress",
        "Performance regression testing",
        (const char *[]){"routes/cross_country/*.json",
                         "routes/stress/*.json"},
        2
    }
};
```

## Skill Specification

### Skill: `/velo-route-bench`

**Location:** `.claude/skills/velo-route-bench/skill.md`

**Trigger:** Use when asked to:
- Improve Velo routing quality or performance
- Debug a routing issue
- Compare routing algorithms
- Validate route correctness
- Find performance regressions

### Workflow

#### Phase 1: Identify Issues

```bash
# Build the benchmark tool
cd velo && make route-bench

# Run quick suite to identify issues
./velo-route-bench --suite quick --graph data/hungary.vlg -v

# Parse output for failures
./velo-route-bench --suite full --graph data/hungary.vlg | \
  jq '.routes[] | select(.validation.overall != "pass")'
```

**Issue categories:**

| Category | JSON Path | Severity |
|----------|-----------|----------|
| Route not found | `.result.status != "ok"` | Critical |
| Distance mismatch | `.validation.distance.pass == false` | Critical |
| Duration mismatch | `.validation.duration.pass == false` | Warning |
| Profile violation | `.validation.profile.pass == false` | Critical |
| Algorithm inconsistency | `.validation.consistency.pass == false` | Critical |
| Slow query | `.performance.query_time_ms > threshold` | Warning |
| High node count | `.performance.nodes_per_km > 500` | Warning |

#### Phase 2: Diagnose Root Cause

**Symptom → Source File Mapping:**

| Symptom | Primary File | Function | Secondary |
|---------|--------------|----------|-----------|
| Route not found | `vl_route.c` | `vl_route()` | `vl_graph.c:vl_graph_nearest_node()` |
| Wrong distance | `vl_route.c` | `reconstruct_path()` | `vl_graph.c:edge distances` |
| Profile violation | `vl_route.c` | `is_edge_allowed()` | `vl_pbf.c:parse_way_tags()` |
| A* finds wrong route | `vl_route.c` | `haversine_heuristic()` | Heuristic inadmissible |
| ALT slower than A* | `vl_landmarks.c` | `landmark_heuristic()` | Bad landmark placement |
| Too many nodes explored | `vl_landmarks.c` | `vl_landmarks_create()` | Not enough landmarks |
| Slow heap operations | `vl_heap.c` | `sh_heap_push/pop` | Consider bucket heap |
| High memory usage | `vl_graph.c` | Graph structure | `vl_landmarks.c` |
| Inconsistent algorithms | `vl_route.c` | Algorithm-specific bug | Compare implementations |

**Debugging commands:**

```bash
# Verbose single route (shows algorithm steps)
./velo-route-bench routes/failing_route.json --graph data/hungary.vlg -v 2>&1 | less

# Compare algorithms on failing route
./velo-route-bench routes/failing_route.json --compare-algorithms --graph data/hungary.vlg

# Check profile compliance in detail
./velo-route-bench routes/failing_route.json --graph data/hungary.vlg | \
  jq '.validation.profile.violation_details'

# Export route geometry for visualization
./velo-route-bench routes/failing_route.json --graph data/hungary.vlg | \
  jq '.result.geometry' > route.geojson
```

#### Phase 3: Fix and Verify

```bash
# After making changes, rebuild
make clean && make lib && make route-bench

# Re-run the failing route
./velo-route-bench routes/failing_route.json --graph data/hungary.vlg -v

# Run full regression suite
./velo-route-bench --suite full --graph data/hungary.vlg

# Compare performance before/after
./velo-route-bench --suite stress --graph data/hungary.vlg --iterations 5 > after.json
# (compare with baseline)
```

#### Phase 4: Add Regression Test

If a bug was found and fixed:

1. **Add unit test to `tests/test_velo.c`:**
```c
static void test_regression_issue_NNN(void) {
    printf("\n=== Regression: Issue NNN - description ===\n");

    // Create minimal graph that reproduces the issue
    VLGraph *graph = create_test_graph_for_issue_NNN();

    VLRoute route;
    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.profile = VL_PROFILE_TRUCK;  // Profile that triggered bug

    VLStatus status = vl_route(graph, source, target, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, expected_distance, 0.1);
    // Add specific assertion for the bug

    vl_free_route(&route);
    vl_graph_free(graph);
}
```

2. **Add benchmark route if applicable:**
```json
// routes/regressions/issue_NNN.json
{
  "name": "Regression test for issue NNN",
  "description": "Route that triggered bug in profile filtering",
  ...
}
```

### Diagnostic Checklists

#### Checklist: Route Not Found

- [ ] Are origin/destination within graph bounds?
- [ ] Is there a connected path between them?
- [ ] Is the profile too restrictive?
- [ ] Check `vl_graph_nearest_node()` returns valid nodes
- [ ] Check graph has edges (not just nodes)

#### Checklist: Wrong Distance

- [ ] Compare Dijkstra vs A* - should be identical
- [ ] Check edge distances in graph (unit: millimeters)
- [ ] Check coordinate projection (lat/lon → meters)
- [ ] Verify `reconstruct_path()` follows parent pointers correctly

#### Checklist: Profile Violation

- [ ] Check `is_edge_allowed()` logic for this profile
- [ ] Verify edge flags are set correctly during PBF parsing
- [ ] Check if way tags are being parsed (`highway=`, `hgv=`, etc.)
- [ ] Verify profile enum values match expected behavior

#### Checklist: Slow Performance

- [ ] Is landmark preprocessing done?
- [ ] How many landmarks? (16 recommended for country-scale)
- [ ] Check landmark placement (should be geographically spread)
- [ ] Profile with `perf` to find hotspots
- [ ] Check heap implementation (binary vs bucket)

## Implementation Plan

### Phase 1: Core Infrastructure (4-6 hours)

**Files to create:**
- `benchmarks/velo_route_bench.c` - Main CLI tool
- `benchmarks/routes/` - Directory structure

**Implement:**
1. CLI argument parsing (use `sh_args.h`)
2. Route JSON loading
3. Basic benchmarking loop
4. JSON output generation

**Milestone:** Can run `./velo-route-bench route.json --graph data.vlg`

### Phase 2: Validation Logic (3-4 hours)

**Implement:**
1. Distance validation with tolerance
2. Duration validation
3. Profile compliance checking
4. Algorithm consistency checking

**Milestone:** Full validation output in JSON

### Phase 3: Benchmark Routes (2-3 hours)

**Create:**
1. 5 urban routes
2. 5 regional routes
3. 5 cross-country routes
4. 5 edge case routes
5. 5 profile-specific routes

**For each route:**
1. Define origin/destination
2. Run exhaustive Dijkstra to get reference
3. Define assertions
4. Set appropriate tolerances

**Milestone:** 25 benchmark routes with references

### Phase 4: Performance Metrics (2-3 hours)

**Implement:**
1. Query timing with warmup
2. Node/edge counting
3. Memory measurement
4. Algorithm comparison mode
5. Derived metrics calculation

**Milestone:** Full performance reporting

### Phase 5: Skill Documentation (1-2 hours)

**Create:**
- `.claude/skills/velo-route-bench/skill.md`
- Workflow documentation
- Symptom → file mapping
- Diagnostic checklists

**Milestone:** Skill usable by Claude

### Phase 6: Integration (1-2 hours)

**Add to build:**
1. Makefile targets: `route-bench`, `test-route-bench`
2. CI integration (GitHub Actions)
3. Baseline performance capture

**Milestone:** `make route-bench && make test-route-bench` works

## Success Criteria

### Quality Targets

| Metric | Target |
|--------|--------|
| All benchmark routes pass validation | 100% |
| Distance accuracy vs Dijkstra | <0.01% error |
| Profile compliance | 0 violations |
| Algorithm consistency | All algorithms return same distance |

### Performance Targets

| Metric | Target |
|--------|--------|
| Urban query (<10km) | <10ms |
| Regional query (50-200km) | <50ms |
| Cross-country query (>300km) | <100ms |
| A* speedup vs Dijkstra | >3x |
| ALT speedup vs A* | >2x |

### CI Integration

```yaml
# .github/workflows/velo-bench.yml
name: Velo Route Benchmark
on: [push, pull_request]
jobs:
  bench:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Build
        run: make -C velo route-bench
      - name: Download test data
        run: make download-monaco
      - name: Run smoke suite
        run: ./velo/velo-route-bench --suite smoke --graph data/monaco.vlg
      - name: Check no regressions
        run: ./velo/velo-route-bench --suite quick --graph data/monaco.vlg --strict
```

## Future Enhancements

### Phase 2 Features (Future)

1. **OSRM comparison mode** - Compare against running OSRM server
2. **Visual route comparison** - Generate GeoJSON diff of routes
3. **Turn-by-turn validation** - Check maneuver instructions
4. **Traffic-aware benchmarks** - Test with time-dependent routing
5. **Memory profiling** - Track allocations during routing

### Phase 3 Features (Future)

1. **Continuous benchmarking** - Track performance over commits
2. **Automatic bisection** - Find commit that caused regression
3. **Flamegraph generation** - Profile hotspots automatically
4. **Cross-platform comparison** - Test on different architectures

## References

- Ralph NETLIB Benchmark: `.claude/skills/ralph-benchmark/skill.md`
- Carta Render Benchmark: `carta/benchmarks/bench_carta.c`
- OSRM Benchmark: https://github.com/Project-OSRM/osrm-backend/tree/master/test
- GraphHopper Tests: https://github.com/graphhopper/graphhopper/tree/master/core/src/test

## Changelog

| Date | Author | Change |
|------|--------|--------|
| 2026-02-08 | Claude | Initial roadmap created |
