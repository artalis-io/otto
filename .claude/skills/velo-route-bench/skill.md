# Velo Route Benchmark Skill

Debug and improve Velo routing quality and performance using automated benchmark validation.

**Trigger:** Use when asked to improve Velo routing, debug routing issues, validate route correctness, compare algorithms, or find performance regressions.

## Quick Start

```bash
cd /Users/mark/Desktop/work/artalis-io/otto/velo

# 1. Build the benchmark tool
make route-bench

# 2. Run smoke test (3 routes)
./velo-route-bench --suite smoke --graph ../data/hungary.vlg -v

# 3. Run single route for detailed analysis
./velo-route-bench --graph ../data/hungary.vlg benchmarks/routes/regional/budapest_szeged.json -v

# 4. Compare all algorithms
./velo-route-bench --graph ../data/hungary.vlg --compare-algorithms benchmarks/routes/regional/*.json
```

## Workflow

### Phase 1: Identify Issues

```bash
# Run full suite and save results
./velo-route-bench --suite full --graph ../data/hungary.vlg > results.json 2>progress.log

# Find failing routes
jq '.routes[] | select(.validation.overall != "pass")' results.json

# Find slow routes
jq '.routes[] | select(.performance.query_time_ms > 100)' results.json

# Find profile violations
jq '.routes[] | select(.validation.profile.violations > 0)' results.json

# Find distance mismatches
jq '.routes[] | select(.validation.distance.pass == false)' results.json
```

### Phase 2: Diagnose Root Cause

**Symptom → Source File Mapping:**

| Symptom | Primary File | Function | Check |
|---------|--------------|----------|-------|
| Route not found | `vl_route.c` | `vl_route()` | Graph connectivity |
| Wrong distance | `vl_route.c` | `reconstruct_path()` | Edge distance units |
| Profile violation | `vl_route.c` | `is_edge_allowed()` | Profile filtering logic |
| A* suboptimal | `vl_route.c` | `haversine_heuristic()` | Heuristic admissibility |
| ALT slow | `vl_landmarks.c` | `landmark_heuristic()` | Landmark placement |
| Too many nodes | `vl_landmarks.c` | `vl_landmarks_create()` | Landmark count |
| Slow heap | `vl_heap.c` | `sh_heap_push/pop` | Heap implementation |
| Inconsistent algos | `vl_route.c` | Multiple | Algorithm-specific bug |

**Debugging commands:**

```bash
# Verbose single route
./velo-route-bench benchmarks/routes/failing.json --graph ../data/hungary.vlg -v 2>&1

# Compare algorithms on failing route
./velo-route-bench benchmarks/routes/failing.json --graph ../data/hungary.vlg --compare-algorithms

# Check profile violations in detail
./velo-route-bench benchmarks/routes/failing.json --graph ../data/hungary.vlg | \
  jq '.routes[0].validation.profile'
```

### Phase 3: Fix and Verify

```bash
# After making changes, rebuild
make clean && make lib && make route-bench

# Re-run the failing route
./velo-route-bench benchmarks/routes/failing.json --graph ../data/hungary.vlg -v

# Run full regression suite
./velo-route-bench --suite full --graph ../data/hungary.vlg

# Compare performance before/after
./velo-route-bench --suite full --graph ../data/hungary.vlg --iterations 3 > after.json
```

### Phase 4: Add Regression Test

If a bug was found and fixed:

1. **Add unit test to `tests/test_velo.c`:**
```c
static void test_regression_issue_NNN(void) {
    printf("\n=== Regression: Issue NNN ===\n");

    // Create test scenario that reproduces the bug
    VLGraph *graph = /* load or create test graph */;
    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.profile = VL_PROFILE_TRUCK;  // Profile that triggered bug

    VLRoute route;
    VLStatus status = vl_route(graph, source, target, &opts, &route);

    ASSERT_EQ(status, VL_OK);
    ASSERT_NEAR(route.distance_m, expected_distance, 0.1);

    vl_free_route(&route);
    vl_graph_free(graph);
}
```

2. **Add benchmark route:**
```json
// benchmarks/routes/regressions/issue_NNN.json
{
  "name": "Regression test for issue NNN",
  "description": "Route that triggered bug in profile filtering",
  ...
}
```

## Diagnostic Checklists

### Route Not Found
- [ ] Are origin/destination within graph bounds?
- [ ] Is there a connected path between them?
- [ ] Is the profile too restrictive?
- [ ] Check `vl_graph_nearest_node()` returns valid nodes
- [ ] Check graph has edges (not just nodes)

### Wrong Distance
- [ ] Compare Dijkstra vs A* - should be identical
- [ ] Check edge distances in graph (unit: millimeters)
- [ ] Check coordinate projection (lat/lon → meters)
- [ ] Verify `reconstruct_path()` follows parent pointers correctly

### Profile Violation
- [ ] Check `is_edge_allowed()` logic for this profile
- [ ] Verify edge flags are set correctly during PBF parsing
- [ ] Check if way tags are being parsed (`highway=`, `hgv=`, etc.)
- [ ] Verify profile enum values match expected behavior

### Slow Performance
- [ ] Is landmark preprocessing done?
- [ ] How many landmarks? (16 recommended for country-scale)
- [ ] Check landmark placement (should be geographically spread)
- [ ] Profile with `perf` to find hotspots
- [ ] Check heap implementation (binary vs bucket)

### Algorithm Inconsistency
- [ ] All algorithms should return same optimal distance
- [ ] A* should explore fewer nodes than Dijkstra
- [ ] Bidirectional should explore fewer than unidirectional
- [ ] Check for floating point precision issues

## Benchmark Suites

| Suite | Routes | Purpose | Time |
|-------|--------|---------|------|
| `smoke` | 3 | Quick sanity check | ~1s |
| `quick` | ~10 | Fast CI validation | ~5s |
| `full` | 25+ | Complete validation | ~30s |

## Route Categories

| Category | Description | Files |
|----------|-------------|-------|
| `urban` | City routes <10km | `routes/urban/*.json` |
| `regional` | Within-country 50-200km | `routes/regional/*.json` |
| `cross_country` | Long routes >300km | `routes/cross_country/*.json` |
| `edge_cases` | Tricky scenarios | `routes/edge_cases/*.json` |
| `profiles` | Profile-specific tests | `routes/profiles/*.json` |
| `regressions` | Bug regression tests | `routes/regressions/*.json` |

## Performance Targets

| Metric | Target | Acceptable | Needs Work |
|--------|--------|------------|------------|
| Urban query | <10ms | <50ms | >100ms |
| Regional query | <50ms | <100ms | >200ms |
| Cross-country query | <100ms | <200ms | >500ms |
| Nodes/km (A* bidir) | <200 | <500 | >1000 |
| Speedup vs Dijkstra | >3x | >2x | <1.5x |

## JSON Output Fields

Key fields to check in benchmark output:

```json
{
  "validation": {
    "overall": "pass|fail",
    "distance": {
      "pass": true,
      "error_pct": 0.0
    },
    "duration": {
      "pass": true,
      "error_pct": 0.06
    },
    "profile": {
      "pass": true,
      "violations": 0
    },
    "consistency": {
      "pass": true,
      "algorithms_tested": 4
    }
  },
  "performance": {
    "query_time_ms": 23.4,
    "nodes_explored": 42315,
    "ms_per_km": 0.134,
    "nodes_per_km": 242
  }
}
```

## CLI Reference

```bash
./velo-route-bench [OPTIONS] [ROUTE_FILES...]

OPTIONS:
  -g, --graph FILE         Graph file (.vlg or .osm.pbf) [required]
  -o, --output FILE        Output JSON file (default: stdout)
  -v, --verbose            Print progress to stderr
  --suite SUITE            Run suite (smoke/quick/full)
  --compare-algorithms     Test all algorithms
  --landmarks N            Use N landmarks (default: 16)
  --iterations N           Run each route N times
  --distance-tolerance PCT Distance error tolerance (default: 0.1%)
  --osrm-url URL           Compare against OSRM server
  --strict                 Fail on any warning
```

## OSRM Comparison

Compare Velo results against a running OSRM server (similar to Ralph vs GLPK):

```bash
# Start OSRM (if you have it running)
./velo-route-bench --graph data/hungary.vlg --osrm-url http://localhost:5000 \
    benchmarks/routes/regional/budapest_szeged.json -v
```

Output includes OSRM comparison in validation:
```json
{
  "osrm": {
    "available": true,
    "pass": true,
    "osrm_distance_m": 174832.0,
    "velo_distance_m": 174850.0,
    "distance_diff_pct": 0.01
  }
}
```

**Requirements:** `curl` must be installed. OSRM profiles map as:
- car/truck/any → driving
- bike → cycling
- foot → walking

## Checklist Before Committing

- [ ] `make test` passes (51+ tests)
- [ ] `./velo-route-bench --suite smoke --graph data.vlg` shows no regressions
- [ ] No new compiler warnings with `-Wall -Wextra`
- [ ] Added regression test for any bug fixes
- [ ] Updated CLAUDE.md if API changed

## Related Documentation

- **Velo Roadmap:** `docs/roadmaps/velo.md` - Routing engine roadmap
- **Velo CLAUDE.md:** `velo/CLAUDE.md` - Library architecture
- **Existing benchmarks:** `velo/benchmarks/bench_routing.c` - Algorithm benchmarks
