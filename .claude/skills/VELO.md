# Velo Routing Engine - Usage Guide

## Overview

Velo is a zero-dependency OSM routing engine that parses PBF files and computes routes using Dijkstra, A*, bidirectional search, and ALT (A* with Landmarks) algorithms.

## Quick Start

```bash
# Build and test
cd velo
make all && make test

# Run benchmarks
make bench
```

## Building Velo

### Basic Build

```bash
cd velo
make all       # Build library + tests
make lib       # Build libvelo.a only
make test      # Run test suite (47 tests)
make bench     # Build and run benchmarks
make debug     # Debug build with symbols
make wasm      # WebAssembly (needs Emscripten)
make clean     # Remove build artifacts
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
│   ├── vl_bucket_heap.c # Bucket-based priority queue
│   ├── vl_pbf.c      # PBF file parsing (uses shared/sh_protobuf, sh_inflate, sh_pbf)
│   ├── vl_graph.c    # Graph construction
│   ├── vl_route.c    # Routing algorithms
│   ├── vl_landmarks.c # ALT algorithm
│   └── velo.c        # Main API
├── api/              # Route server REST API
├── tests/            # Test suite
└── benchmarks/       # Performance tests
```

**Note:** Protobuf decoding and zlib decompression are provided by the shared library (`sh_protobuf.h`, `sh_inflate.h`, `sh_pbf.h`).

## Using Velo in Your Code

### Basic Routing

```c
#include "velo.h"

int main() {
    // Load graph from PBF file
    VLGraph *graph = vl_load_pbf("map.osm.pbf");
    if (!graph) {
        fprintf(stderr, "Failed to load PBF\n");
        return 1;
    }

    // Define origin and destination
    SHCoord origin = {47.497, 19.040};  // Budapest
    SHCoord dest = {46.253, 20.148};    // Szeged

    // Compute route (defaults: fastest, car profile)
    VLRoute route;
    int result = vl_route_coords(graph, origin, dest, NULL, &route);

    if (result == 0) {
        printf("Distance: %.1f km\n", route.distance_m / 1000.0);
        printf("Duration: %.1f min\n", route.duration_s / 60.0);
    }

    vl_free_route(&route);
    vl_graph_free(graph);
    return 0;
}
```

### Routing with Options

```c
#include "velo.h"

int main() {
    VLGraph *graph = vl_load_pbf("map.osm.pbf");

    // Configure routing options
    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.weight = VL_WEIGHT_DISTANCE;  // Shortest (vs VL_WEIGHT_DURATION for fastest)
    opts.profile = VL_PROFILE_TRUCK;   // Avoid hgv=no roads
    opts.include_geometry = 1;         // Include route geometry

    SHCoord origin = {47.497, 19.040};
    SHCoord dest = {46.253, 20.148};

    VLRoute route;
    vl_route_coords(graph, origin, dest, &opts, &route);

    // Access route geometry
    for (int i = 0; i < route.num_coords; i++) {
        printf("Point %d: %.6f, %.6f\n", i, route.coords[i].lat, route.coords[i].lon);
    }

    vl_free_route(&route);
    vl_graph_free(graph);
    return 0;
}
```

### Using Landmarks for Fast Routing

```c
#include "velo.h"

int main() {
    VLGraph *graph = vl_load_pbf("map.osm.pbf");

    // Build indices (required for landmarks)
    vl_graph_build_grid_index(graph);
    vl_graph_build_reverse_index(graph);

    // Create landmarks for fast A* (16-32 recommended)
    VLLandmarks *lm = vl_landmarks_create(graph, 16);

    // Find nearest nodes
    uint32_t from = vl_graph_nearest_node(graph, 47.497, 19.040);
    uint32_t to = vl_graph_nearest_node(graph, 46.253, 20.148);

    // Route using landmarks (5-10x faster than plain A*)
    VLRouteOptions opts;
    vl_default_options(&opts);

    VLRoute route;
    vl_route_astar_landmarks(graph, lm, from, to, &opts, &route);

    printf("Route: %.1f km in %.1f min\n",
           route.distance_m / 1000.0, route.duration_s / 60.0);

    vl_landmarks_free(lm);
    vl_free_route(&route);
    vl_graph_free(graph);
    return 0;
}
```

### Compiling Your Program

```bash
# Compile with Velo (requires shared and miniz)
gcc -O3 -I./include -I../shared/include -I../vendor/miniz \
    myprogram.c -L. -lvelo -L../shared -lshared -L../vendor/miniz -lminiz -lm \
    -o myprogram
```

## API Reference

### Graph Loading
- `vl_load_pbf(path)` - Load graph from OSM PBF file
- `vl_load_binary(path)` - Load from binary format (faster)
- `vl_save_binary(graph, path)` - Save to binary format
- `vl_graph_free(graph)` - Free graph resources

### Index Building
- `vl_graph_build_grid_index(graph)` - Build spatial index for nearest-node queries
- `vl_graph_build_reverse_index(graph)` - Build reverse edges (for bidirectional search)

### Routing
- `vl_route_coords(graph, origin, dest, opts, route)` - Route by coordinates
- `vl_route_dijkstra(graph, from, to, opts, route)` - Dijkstra's algorithm
- `vl_route_astar(graph, from, to, opts, route)` - A* algorithm
- `vl_route_astar_bidir(graph, from, to, opts, route)` - Bidirectional A*
- `vl_route_astar_landmarks(graph, lm, from, to, opts, route)` - A* with landmarks

### Landmarks
- `vl_landmarks_create(graph, count)` - Create landmark set
- `vl_landmarks_free(lm)` - Free landmarks

### Utilities
- `vl_graph_nearest_node(graph, lat, lon)` - Find nearest node to coordinate
- `vl_free_route(route)` - Free route resources
- `vl_default_options(opts)` - Initialize options with defaults

## Vehicle Profiles

| Profile | Constant | Description | Avoids |
|---------|----------|-------------|--------|
| Car | `VL_PROFILE_CAR` | Standard car | Nothing |
| Truck | `VL_PROFILE_TRUCK` | HGV/truck | `hgv=no`, residential, service |
| Bike | `VL_PROFILE_BIKE` | Bicycle | Motorways, trunk roads |
| Foot | `VL_PROFILE_FOOT` | Pedestrian | Motorways, trunk, primary |
| Any | `VL_PROFILE_ANY` | No filtering | Nothing |

## Routing Modes

| Mode | Constant | Description |
|------|----------|-------------|
| Fastest | `VL_WEIGHT_DURATION` | Minimize travel time |
| Shortest | `VL_WEIGHT_DISTANCE` | Minimize total distance |

## Performance Notes

| Operation | Time | Notes |
|-----------|------|-------|
| PBF parse (Hungary, 300MB) | ~10s | One-time |
| Binary graph load | ~100ms | 10-100x faster than PBF |
| Landmark creation (16 landmarks) | ~4s | One-time |
| Route with landmarks | 30-50ms | Country-scale |
| Route without landmarks | 150-350ms | Country-scale |

## Common Issues

### Memory for landmarks
Dual-mode landmarks (distance + time) use ~2.6GB for 2.7M nodes with 32 landmarks. Use fewer landmarks or compile with `-DVL_LANDMARKS_NO_TRANSPOSE` to halve memory (10% slower queries).

### Bidirectional search heuristic
Bidirectional A* requires a consistent heuristic. Velo uses the standard potential function; modifying the heuristic may break optimality.

### No route found
- Check coordinates are within graph bounds
- Try different profile (some roads are restricted)
- Verify graph has connected components

## Dependencies

- **External**: None
- **Vendored**: ../vendor/miniz (public domain zlib)
- **Shared**: ../shared (geo utilities)
- **Standard Library**: stdio, stdlib, string, math
