# Velo - Zero-Dependency OpenStreetMap Routing Engine

**V**ery **E**fficient **L**ocation **O**ptimizer

A zero-dependency routing engine that parses OSM PBF files and computes shortest/fastest routes using bidirectional Dijkstra and A* algorithms.

## Features

- **Zero dependencies**: No external libraries (miniz included for zlib decompression)
- **OSM PBF parsing**: Direct parsing of OpenStreetMap Protocol Buffer files
- **Multiple algorithms**: Dijkstra, A*, and bidirectional variants
- **Fast**: Country-scale routing in < 100ms
- **Portable**: Compiles to native code and WebAssembly
- **Memory efficient**: CSR graph format for compact storage

## Quick Start

```bash
# Build library and tests
make all

# Run tests
make test

# Download a map and run benchmarks
./scripts/download-map.sh hungary
./scripts/convert-map.sh hungary-latest.osm.pbf
make bench && ./bench_routing hungary.vlg
```

## Getting Map Data

Velo uses OpenStreetMap PBF files, available from [Geofabrik](https://download.geofabrik.de/).

### Download Maps

```bash
# Download Hungary (~294 MB)
./scripts/download-map.sh hungary

# Download other countries
./scripts/download-map.sh germany
./scripts/download-map.sh france

# Download regions with full path
./scripts/download-map.sh north-america/us/california
```

### Convert to Binary Format

Converting to `.vlg` format enables 100x faster loading:

```bash
# Convert PBF to VLG (takes 10-30s depending on size)
./scripts/convert-map.sh hungary-latest.osm.pbf

# Custom output name
./scripts/convert-map.sh hungary-latest.osm.pbf hungary.vlg
```

### Sample Map Sizes

| Region | PBF Size | VLG Size | Nodes | Edges |
|--------|----------|----------|-------|-------|
| Hungary | 294 MB | 125 MB | 2.7M | 5.5M |
| Germany | 3.9 GB | ~1.5 GB | ~35M | ~70M |
| California | 1.1 GB | ~400 MB | ~10M | ~20M |

## Usage

### Loading from PBF

```c
#include "velo.h"

// Load graph from OSM PBF file
VLGraph *graph = vl_load_pbf("hungary.osm.pbf");

// Route between coordinates
VLCoord origin = {47.4979, 19.0402};      // Budapest
VLCoord destination = {46.2530, 20.1414}; // Szeged

VLRouteOptions opts;
vl_default_options(&opts);

VLRoute route;
if (vl_route_coords(graph, origin, destination, &opts, &route) == VL_OK) {
    printf("Distance: %.1f km\n", route.distance_m / 1000.0);
    printf("Duration: %.1f min\n", route.duration_s / 60.0);
}

vl_free_route(&route);
vl_graph_free(graph);
```

### Using Preprocessed Binary

```c
// Save graph to binary for faster loading
vl_save_binary(graph, "hungary.vlg");

// Later: load binary (much faster than PBF)
VLGraph *graph = vl_load_binary("hungary.vlg");
```

### Algorithm Selection

```c
VLRouteOptions opts;
vl_default_options(&opts);

// Choose algorithm
opts.algorithm = VL_ALGORITHM_ASTAR_BIDIR;  // Default, fastest
// opts.algorithm = VL_ALGORITHM_ASTAR;
// opts.algorithm = VL_ALGORITHM_DIJKSTRA_BIDIR;
// opts.algorithm = VL_ALGORITHM_DIJKSTRA;

// Optimize for distance or duration
opts.weight = VL_WEIGHT_DURATION;  // Default
// opts.weight = VL_WEIGHT_DISTANCE;

// Include path geometry
opts.include_geometry = 1;  // Default
```

## API Reference

### High-Level Functions

```c
// Load graph from PBF file
VLGraph *vl_load_pbf(const char *filename);

// Load/save binary format
VLGraph *vl_load_binary(const char *filename);
VLStatus vl_save_binary(const VLGraph *graph, const char *filename);

// Route between node indices
VLStatus vl_route(const VLGraph *graph, uint32_t source, uint32_t target,
                  const VLRouteOptions *opts, VLRoute *route);

// Route between coordinates
VLStatus vl_route_coords(const VLGraph *graph, VLCoord origin, VLCoord destination,
                         const VLRouteOptions *opts, VLRoute *route);

// Cleanup
void vl_free_route(VLRoute *route);
void vl_graph_free(VLGraph *graph);
```

### Low-Level Functions

```c
// PBF parsing
VLPBFContext *vl_pbf_context_create(void);
VLStatus vl_pbf_parse_file(VLPBFContext *ctx, const char *filename);
void vl_pbf_context_free(VLPBFContext *ctx);

// Graph building
VLGraphBuilder *vl_graph_builder_create(size_t expected_nodes);
VLStatus vl_graph_build_from_pbf(VLGraphBuilder *builder, const VLPBFContext *ctx);
VLGraph *vl_graph_finalize(VLGraphBuilder *builder);
void vl_graph_builder_free(VLGraphBuilder *builder);

// Graph queries
uint32_t vl_graph_nearest_node(const VLGraph *graph, VLCoord coord);
void vl_graph_stats(const VLGraph *graph, uint32_t *num_nodes, uint32_t *num_edges,
                    uint32_t *max_out_degree, double *avg_out_degree);

// Utilities
double vl_haversine(VLCoord a, VLCoord b);
const char *vl_status_string(VLStatus status);
```

## Data Structures

### Route Result

```c
typedef struct {
    VLStatus status;
    double distance_m;      // Total distance in meters
    double duration_s;      // Total duration in seconds
    int num_coords;         // Polyline point count
    VLCoord *coords;        // Path geometry (if requested)
    int num_nodes;          // Path node count
    uint32_t *node_indices; // Path as node indices
    uint32_t nodes_explored;
    double search_time_ms;
} VLRoute;
```

### Coordinate

```c
typedef struct {
    double lat;
    double lon;
} VLCoord;
```

## Performance

Baseline performance on Hungary (2.7M nodes, 5.5M edges):

| Operation | Time |
|-----------|------|
| PBF parse + build | ~8s |
| Binary graph load (mmap) | 116ms |
| A* bidirectional | 150-350ms |

### Optimizations

Optional preprocessing can significantly speed up queries:

| Optimization | Preprocessing | Query Speedup |
|--------------|--------------|---------------|
| Hilbert reordering | 648ms | 1.5-2.1x |
| ALT (16 landmarks) | 2.2s | 2.5-3.8x |
| Combined | ~3s | 3-4x |

See [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) for detailed benchmarks.

```c
// Enable Hilbert reordering (cache-friendly layout)
vl_graph_reorder_hilbert(graph);

// Enable ALT (landmark-based heuristic)
VLLandmarks *lm = vl_landmarks_create(graph, 16);
vl_route_astar_landmarks(graph, lm, src, dst, &opts, &route);
```

## File Formats

### Binary Graph (.vlg)

Preprocessed graph format for fast loading:
- Magic number: "VELG"
- Header with node/edge counts and bounding box
- Node array (coordinates, edge offsets, OSM IDs)
- Edge array (targets, distances, durations, flags)

### PBF Parsing

Supported OSM elements:
- Nodes (DenseNodes format)
- Ways with highway=* tags
- One-way tags
- Maxspeed tags

Supported highway types:
- motorway, trunk, primary, secondary, tertiary
- residential, living_street, unclassified, service

## Building

### Requirements

- C11 compiler (GCC, Clang, MSVC)
- Make (optional)

### Native Build

```bash
make all           # Library and tests
make test          # Run tests
make bench         # Build benchmarks
make debug         # Debug build
```

### WebAssembly (Emscripten)

```bash
make wasm
```

## License

MIT License - see LICENSE file.

## See Also

- [carta](../carta/) - Map tile generator
- [shared](../shared/) - Shared protobuf/inflate/PBF parsing library (used by both velo and carta)
- [fuelwise](../fuelwise/) - Refueling optimization

## Acknowledgments

- miniz: Public domain zlib implementation by Rich Geldreich
- OpenStreetMap contributors for map data
