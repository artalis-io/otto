# Velo Route Server - Usage Guide

## Overview

HTTP routing API server built on Velo. Provides route planning between coordinates with vehicle profiles, optimization modes, and Google Polyline encoded geometry. Compatible with mapping libraries like Leaflet and Google Maps.

## Quick Start

```bash
# Download OSM data first
../scripts/download-osm.sh hungary

# Build and run
cd velo/api
make
./velo-route-server ../data/hungary-latest.osm.pbf

# Test
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1'
```

## Building the Server

```bash
cd velo/api
make              # Build server
make deps         # Build velo dependency
make debug        # Debug build
make clean        # Remove artifacts
make run          # Run with Hungary data
make test         # Integration tests
```

## Directory Structure

```
velo/api/
├── src/
│   ├── main.c        # HTTP server and API handlers
│   ├── polyline.c    # Google Polyline encoding
│   └── polyline.h    # Polyline header
├── Makefile
└── CLAUDE.md
```

## API Endpoints

### Health Check

```bash
GET /api/v1/health

Response:
{"status": "ok"}
```

### Graph Statistics

```bash
GET /api/v1/stats

Response:
{
  "nodes": 2745123,
  "edges": 6234567,
  "bounds": {
    "min_lat": 45.737,
    "max_lat": 48.585,
    "min_lon": 16.114,
    "max_lon": 22.898
  }
}
```

### Calculate Route

```bash
GET /api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=car&mode=fastest

Response:
{
  "status": "ok",
  "route": {
    "distance": 187432.5,
    "duration": 7234.2,
    "profile": "car",
    "mode": "fastest",
    "from": [47.497, 19.040],
    "to": [46.253, 20.148],
    "geometry": "_p~iF~ps|U_ulLnnqC_mqNvxq`@"
  },
  "meta": {
    "nodes_explored": 12543,
    "search_time_ms": 34.5
  }
}
```

### Route Parameters

| Parameter | Values | Default | Description |
|-----------|--------|---------|-------------|
| `from` | lat,lon | required | Origin coordinates |
| `to` | lat,lon | required | Destination coordinates |
| `profile` | car, truck, bike, foot | car | Vehicle type |
| `mode` | fastest, shortest | fastest | Optimization mode |
| `geometry` | true, false | true | Include polyline geometry |

### POST Route (for long parameter lists)

```bash
POST /api/v1/route
Content-Type: application/json

{
  "from": [47.5, 19.0],
  "to": [46.2, 20.1],
  "profile": "truck",
  "mode": "shortest",
  "geometry": true
}
```

## Configuration

### Environment Variables

```bash
ROUTE_GRAPH_PATH=/data/map.osm.pbf    # Path to graph file
ROUTE_PORT=8082                        # Server port (default: 8082)
ROUTE_HOST=0.0.0.0                     # Bind host (default: 0.0.0.0)
ROUTE_LANDMARKS=1                      # Enable landmarks (0/1)
ROUTE_LANDMARK_COUNT=32                # Number of landmarks
```

### Command Line Options

```bash
./velo-route-server [options] <graph-file>

Options:
  -p, --port PORT       Server port (default: 8082)
  -h, --host HOST       Bind host (default: 0.0.0.0)
  -c, --config FILE     Config file
  --no-landmarks        Disable ALT algorithm
  --landmarks N         Number of landmarks (default: 32)
```

### Config File

```yaml
# config.yaml
pbf_path: /data/hungary-latest.osm.pbf
port: 8082
host: 0.0.0.0
landmarks: true
landmark_count: 32
```

## Vehicle Profiles

| Profile | Avoids |
|---------|--------|
| `car` | Nothing (all roads) |
| `truck` | Roads with `hgv=no`, residential, service roads |
| `bike` | Motorways, trunk roads |
| `foot` | Motorways, trunk roads, primary roads |

## Routing Modes

| Mode | Description |
|------|-------------|
| `fastest` | Minimize travel time (considers speed limits) |
| `shortest` | Minimize total distance |

## Response Format

### Success Response

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

### Error Response

```json
{"error": "No route found"}
```

### Status Codes

| Code | Description |
|------|-------------|
| 200 | Success |
| 400 | Bad request (invalid parameters) |
| 404 | No route found |
| 503 | Graph not loaded |

## Google Polyline Format

The `geometry` field contains a Google Polyline encoded string:
- Precision 5 (standard) = 5 decimal places = ~1.1m accuracy
- Compact encoding for efficient transmission
- Decode using Google Maps JavaScript API or polyline libraries

### Decoding Example (JavaScript)

```javascript
// Using @mapbox/polyline
const polyline = require('@mapbox/polyline');
const coords = polyline.decode(response.route.geometry);
// coords = [[lat1, lon1], [lat2, lon2], ...]
```

## Examples

### Basic Route

```bash
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1'
```

### Truck Route (Shortest)

```bash
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=truck&mode=shortest'
```

### Without Geometry

```bash
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&geometry=false'
```

## Performance Notes

| Operation | Time |
|-----------|------|
| Landmark creation | ~4s (one-time) |
| With 32 landmarks | 30-50ms queries |
| Without landmarks | 150-350ms queries |
| Binary graph load | ~100ms |
| PBF parse | ~8s |

## Common Issues

### Port already in use

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

- **Velo**: Routing engine library
- **mongoose**: HTTP server (vendor)
- **pthread**: Thread support
