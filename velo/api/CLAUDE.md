# Velo Route Server - Claude Instructions

## Overview

HTTP routing API server built on velo. Provides route planning between coordinates with vehicle profiles, optimization modes, and Google Polyline encoded geometry.

## Quick Start

```bash
# Build and run
make
./velo-route-server ../data/hungary-latest.osm.pbf

# Test
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1'
```

## Directory Structure

```
route-server/
├── src/
│   ├── main.c        # HTTP server and API handlers
│   ├── polyline.c    # Google Polyline encoding
│   └── polyline.h    # Polyline header
├── Makefile
├── README.md
└── CLAUDE.md         # This file
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Graph statistics |
| `/api/v1/route` | GET/POST | Calculate route |

## Route Parameters

| Parameter | Values | Default | Description |
|-----------|--------|---------|-------------|
| `from` | lat,lon | required | Origin |
| `to` | lat,lon | required | Destination |
| `profile` | car, truck, bike, foot | car | Vehicle type |
| `mode` | fastest, shortest | fastest | Optimization |
| `geometry` | true, false | true | Include polyline |

## Key Components

### main.c

- Configuration loading (env vars, CLI, YAML file)
- Mongoose HTTP server setup
- Request routing and parameter parsing
- Velo graph loading and landmark creation
- Route calculation using velo API
- JSON response formatting

### polyline.c

Google Polyline Algorithm implementation:
- `polyline_encode()` - Encode coordinate array to string
- `polyline_decode()` - Decode string to coordinates
- `polyline_max_encoded_size()` - Calculate buffer size

Precision 5 (standard) = 5 decimal places = ~1.1m accuracy.

## Velo Integration

```c
// Load graph (PBF or binary)
VLGraph *graph = vl_load_pbf("map.osm.pbf");
// or
VLGraph *graph = vl_load_binary("map.vlg");

// Build indices
vl_graph_build_grid_index(graph);
vl_graph_build_reverse_index(graph);

// Create landmarks for fast routing
VLLandmarks *lm = vl_landmarks_create(graph, 32);

// Route with landmarks
VLRouteOptions opts = {
    .algorithm = VL_ALGORITHM_ASTAR_BIDIR,
    .weight_type = VL_WEIGHT_DURATION,  // fastest
    .profile = VL_PROFILE_CAR,
    .include_geometry = 1
};

VLRoute route;
vl_route_astar_landmarks_bidir(graph, lm, from, to, &opts, &route);

// Use route.distance_m, route.duration_s, route.coords
vl_free_route(&route);
```

## Configuration

### Environment Variables

```bash
ROUTE_GRAPH_PATH    # Path to graph file
ROUTE_PORT          # Server port (default: 8082)
ROUTE_HOST          # Bind host (default: 0.0.0.0)
ROUTE_LANDMARKS     # Enable landmarks (0/1)
ROUTE_LANDMARK_COUNT # Number of landmarks
```

### Command Line

```bash
./velo-route-server [options] <graph-file>
  -p, --port PORT       Server port
  -h, --host HOST       Bind host
  -c, --config FILE     Config file
  --no-landmarks        Disable ALT
  --landmarks N         Landmark count
```

## Build Commands

```bash
make          # Build server
make deps     # Build velo dependency
make debug    # Debug build
make clean    # Clean artifacts
make run      # Run with Hungary data
make test     # Integration test
```

## Response Format

```json
{
  "status": "ok",
  "route": {
    "distance": 187432.5,
    "duration": 7234.2,
    "profile": "car",
    "mode": "fastest",
    "from": [47.497, 19.040],
    "to": [46.253, 20.148],
    "geometry": "encoded_polyline..."
  },
  "meta": {
    "nodes_explored": 12543,
    "search_time_ms": 34.5
  }
}
```

## Error Handling

HTTP status codes:
- 200: Success
- 400: Bad request (invalid params)
- 404: No route found
- 503: Graph not loaded

Error response:
```json
{"error": "No route found"}
```

## Performance Notes

- Landmark creation: ~4s one-time cost
- With 32 landmarks: 30-50ms queries
- Without landmarks: 150-350ms queries
- Binary graph loading: ~100ms vs ~8s for PBF

## Common Issues

### Port in use
```bash
./velo-route-server -p 8083 graph.osm.pbf
```

### No route found
- Check coordinates are within graph bounds
- Try different profile (some roads restricted)
- Verify graph has connected components

### Slow queries
Enable landmarks:
```bash
./velo-route-server --landmarks 32 graph.osm.pbf
```

## Dependencies

- **velo** - Routing engine library
- **mongoose** - HTTP server (../vendor/mongoose)
- **pthread** - Thread support
