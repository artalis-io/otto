# Claude Code Instructions for Velo

## Project Overview

Velo is a zero-dependency routing engine that parses OSM PBF files and computes routes using Dijkstra and A* algorithms.

## Quick Start

```bash
# Build and test
make all && make test

# Run benchmarks
make bench
```

## Directory Structure

```
velo/
├── include/          # Public headers
│   ├── velo.h        # Unified API
│   ├── vl_types.h    # Data structures
│   ├── vl_pbf.h      # PBF parsing
│   ├── vl_graph.h    # Graph operations
│   └── vl_route.h    # Routing algorithms
├── src/              # Implementation
│   ├── vl_geo.c      # Haversine, coordinates
│   ├── vl_heap.c     # Binary min-heap
│   ├── vl_protobuf.c # Protobuf decoder
│   ├── vl_inflate.c  # Zlib wrapper
│   ├── vl_pbf.c      # PBF file parsing
│   ├── vl_graph.c    # Graph construction
│   ├── vl_route.c    # Routing algorithms
│   └── velo.c        # Main API
├── ../vendor/        # Shared third-party (miniz)
├── tests/            # Test suite
└── benchmarks/       # Performance tests
```

## Key Files

| File | Purpose |
|------|---------|
| `vl_types.h` | All data structures (VLGraph, VLRoute, VLLandmarks, etc.) |
| `vl_route.c` | Dijkstra and A* implementations |
| `vl_landmarks.c` | ALT algorithm (A* with Landmarks and Triangle inequality) |
| `vl_pbf.c` | PBF parsing (DenseNodes, Ways) |
| `vl_graph.c` | CSR graph construction |

## Build Commands

```bash
make all      # Build library + tests
make lib      # Build libvelo.a only
make test     # Run test suite
make bench    # Build and run benchmarks
make debug    # Debug build with symbols
make wasm     # WebAssembly (needs Emscripten)
make clean    # Remove build artifacts
```

## Architecture

### Data Flow
```
PBF File → vl_pbf.c → VLPBFContext → vl_graph.c → VLGraph → vl_route.c → VLRoute
```

### Graph Format (CSR)
- Nodes: array with edge_start/edge_count
- Edges: contiguous array indexed by node offsets
- Compact and cache-friendly

### Algorithms
1. **Dijkstra**: Standard shortest path
2. **Bidirectional Dijkstra**: Search from both ends
3. **A***: Haversine heuristic for admissibility
4. **Bidirectional A***: Consistent potential function
5. **ALT (A* with Landmarks)**: Precomputed landmark heuristics for faster convergence

### Routing Modes
- **VL_WEIGHT_DISTANCE**: Shortest path (minimizes total distance in meters)
- **VL_WEIGHT_DURATION**: Fastest path (minimizes total time in seconds)

### Vehicle Profiles
| Profile | Description | Avoids |
|---------|-------------|--------|
| `VL_PROFILE_CAR` | Standard car | Nothing (all roads) |
| `VL_PROFILE_TRUCK` | HGV/truck | Roads with `hgv=no`, residential, service |
| `VL_PROFILE_BIKE` | Bicycle | Motorways, trunk roads |
| `VL_PROFILE_FOOT` | Pedestrian | Motorways, trunk roads, primary roads |
| `VL_PROFILE_ANY` | No filtering | Nothing (all roads) |

### Landmarks (ALT Algorithm)
Landmarks precompute shortest path distances to/from strategic nodes for tighter A* heuristics.

**Dual-mode landmarks**: Both distance-based and time-based shortest paths are precomputed:
- Distance landmarks → used for `VL_WEIGHT_DISTANCE` routing
- Time landmarks → used for `VL_WEIGHT_DURATION` routing

This ensures optimal A* heuristics for both routing modes. Memory usage is ~8 arrays × num_landmarks × num_nodes × 8 bytes.

## Testing

```bash
# Run all tests
make test

# Expected: 47 tests pass
```

### Test Categories
- **Geo Tests**: Haversine distance, coordinate validation
- **Heap Tests**: Priority queue operations
- **Protobuf Tests**: Varint encoding, delta decoding
- **Graph Tests**: Graph construction, nearest node
- **Routing Tests**: All algorithms, geometry output
- **Vehicle Profile Tests**: Profile-based edge filtering
- **Shortest vs Fastest Tests**: Verify optimal routing for both modes

## Benchmarks

```bash
# Run routing benchmarks
./bench_routing

# Parse and benchmark a PBF file
./bench_pbf map.osm.pbf output.vlg
```

## Common Issues

1. **miniz compilation**: Ensure vendor/ has all miniz files
2. **mmap on Windows**: Falls back to malloc/fread
3. **Landmark memory**: Dual-mode landmarks use ~2.6GB for 2.7M nodes with 32 landmarks

## Performance Notes

- Binary graph loading is 10-100x faster than PBF parsing
- A* typically explores 2-3x fewer nodes than Dijkstra
- Bidirectional search halves the search space
- ALT (A* with Landmarks) provides 5-10x speedup for long routes
- Landmark preprocessing: ~num_landmarks × 4 Dijkstra runs (distance/time × to/from)
- Compile with `-DVL_LANDMARKS_NO_TRANSPOSE` to halve landmark memory (10% slower queries)

## API Usage

```c
// Basic routing (defaults: fastest, car profile)
VLGraph *graph = vl_load_pbf("map.osm.pbf");
VLRoute route;
vl_route_coords(graph, origin, dest, NULL, &route);
printf("%.1f km in %.1f min\n", route.distance_m / 1000.0, route.duration_s / 60.0);
vl_free_route(&route);
vl_graph_free(graph);

// Shortest route with truck profile
VLRouteOptions opts;
vl_default_options(&opts);
opts.weight = VL_WEIGHT_DISTANCE;  // Shortest (vs VL_WEIGHT_DURATION for fastest)
opts.profile = VL_PROFILE_TRUCK;   // Avoid hgv=no roads
vl_route_coords(graph, origin, dest, &opts, &route);

// Using landmarks for faster A* (recommended for large graphs)
VLLandmarks *lm = vl_landmarks_create(graph, 16);  // 16 landmarks
vl_route_astar_landmarks(graph, lm, source_node, target_node, &opts, &route);
vl_landmarks_free(lm);
```

## Dependencies

- **External**: None
- **Vendored**: ../vendor/miniz (public domain zlib, shared with carta)
- **Standard Library**: stdio, stdlib, string, math
