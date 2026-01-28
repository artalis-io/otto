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
| `vl_types.h` | All data structures (VLGraph, VLRoute, etc.) |
| `vl_route.c` | Dijkstra and A* implementations |
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

## Testing

```bash
# Run all tests
make test

# Expected: 30+ tests pass
```

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
3. **Bidirectional search**: Reverse graph lookup is O(E), could use reverse graph index

## Performance Notes

- Binary graph loading is 10-100x faster than PBF parsing
- A* typically explores 2-3x fewer nodes than Dijkstra
- Bidirectional search halves the search space

## API Usage

```c
// Basic routing
VLGraph *graph = vl_load_pbf("map.osm.pbf");
VLRoute route;
vl_route_coords(graph, origin, dest, NULL, &route);
printf("%.1f km\n", route.distance_m / 1000.0);
vl_free_route(&route);
vl_graph_free(graph);
```

## Dependencies

- **External**: None
- **Vendored**: ../vendor/miniz (public domain zlib, shared with carta)
- **Standard Library**: stdio, stdlib, string, math
