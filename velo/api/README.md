# Velo Route Server

A lightweight HTTP routing API server built on the velo routing engine. Provides fast route planning between coordinates with support for multiple vehicle profiles and optimization modes.

## Quick Start

```bash
# Download OSM data
../scripts/download-osm.sh hungary

# Build and run
make
./velo-route-server ../data/hungary-latest.osm.pbf

# Test routing
curl 'http://localhost:8082/api/v1/route?from=47.497,19.040&to=46.253,20.148&profile=car&mode=fastest'
```

## API Endpoints

### Health Check
```
GET /api/v1/health
```

Response:
```json
{
  "status": "healthy",
  "service": "velo-route-server",
  "version": "0.1.0"
}
```

### Graph Statistics
```
GET /api/v1/stats
```

Response:
```json
{
  "graph_path": "../data/hungary-latest.osm.pbf",
  "num_nodes": 2743806,
  "num_edges": 5518912,
  "landmarks_enabled": true,
  "landmark_count": 32,
  "bbox": {
    "min_lat": 45.737128,
    "min_lon": 16.113386,
    "max_lat": 48.585258,
    "max_lon": 22.896576
  }
}
```

### Calculate Route

```
GET /api/v1/route?from=lat,lon&to=lat,lon&profile=car&mode=fastest&geometry=true
POST /api/v1/route
```

**Query Parameters (GET) / JSON Body (POST):**

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `from` | string | required | Origin coordinates (lat,lon) |
| `to` | string | required | Destination coordinates (lat,lon) |
| `profile` | string | car | Vehicle profile: `car`, `truck`, `bike`, `foot` |
| `mode` | string | fastest | Optimization: `fastest`, `shortest` |
| `geometry` | bool | true | Include encoded polyline |

**Response:**
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
    "geometry": "encoded_polyline_string..."
  },
  "meta": {
    "nodes_explored": 12543,
    "search_time_ms": 34.5
  }
}
```

**POST Example:**
```bash
curl -X POST http://localhost:8082/api/v1/route \
  -H "Content-Type: application/json" \
  -d '{
    "from": "47.497,19.040",
    "to": "46.253,20.148",
    "profile": "car",
    "mode": "fastest"
  }'
```

## Vehicle Profiles

| Profile | Description | Avoids |
|---------|-------------|--------|
| `car` | Standard car routing | - |
| `truck` | Heavy goods vehicle | Residential, service roads |
| `bike` | Bicycle routing | Motorways, trunk roads |
| `foot` | Pedestrian routing | Motorways, trunk, primary roads |

## Optimization Modes

| Mode | Description |
|------|-------------|
| `fastest` | Minimize travel time (uses speed limits) |
| `shortest` | Minimize distance |

## Geometry Encoding

The `geometry` field contains a [Google Encoded Polyline](https://developers.google.com/maps/documentation/utilities/polylinealgorithm) with precision 5 (standard).

**Decoding in JavaScript:**
```javascript
// Using @mapbox/polyline
const polyline = require('@mapbox/polyline');
const coords = polyline.decode(route.geometry);
// coords = [[lat1, lon1], [lat2, lon2], ...]
```

**Decoding in Python:**
```python
import polyline
coords = polyline.decode(route['geometry'])
```

## Command Line Options

```
Usage: velo-route-server [options] <graph-file>

Options:
  -p, --port PORT      Port to listen on (default: 8082)
  -h, --host HOST      Host to bind to (default: 0.0.0.0)
  -c, --config FILE    Configuration file (YAML format)
  --no-landmarks       Disable ALT landmarks (faster startup)
  --landmarks N        Number of landmarks (default: 32)
  --help               Show this help
```

## Configuration

### Environment Variables

```bash
ROUTE_GRAPH_PATH=/data/graph.osm.pbf
ROUTE_PORT=8082
ROUTE_HOST=0.0.0.0
ROUTE_LANDMARKS=1
ROUTE_LANDMARK_COUNT=32
```

### Configuration File

```yaml
# config.yaml
graph_path: /data/hungary-latest.osm.pbf
port: 8082
host: 0.0.0.0
landmarks: 1
landmark_count: 32
name: "Hungary Route Server"
```

## Graph Files

The server accepts two graph formats:

1. **OSM PBF** (`.osm.pbf`) - Parsed on startup, slower to load but standard format
2. **Velo Binary** (`.vlg`) - Pre-processed format, fast loading via mmap

### Converting to Binary Format

```bash
# Using velo CLI
cd ../velo
./velo_cli convert ../data/hungary-latest.osm.pbf ../data/hungary.vlg
```

## Performance

With ALT landmarks (32) on Hungary (2.7M nodes):
- **Startup**: ~8s (PBF) or ~100ms (binary)
- **Landmark creation**: ~4s (one-time)
- **Query time**: 30-50ms

Without landmarks:
- **Startup**: ~8s (PBF) or ~100ms (binary)
- **Query time**: 150-350ms

## Building

```bash
# Build route server
make

# Debug build
make debug

# Clean
make clean
```

## Dependencies

- **velo** - Routing engine
- **Keel** - HTTP server (vendor submodule, MIT)
- POSIX threads (pthread)

## Docker

```dockerfile
FROM debian:bookworm-slim

COPY velo-route-server /app/
COPY data/hungary-latest.osm.pbf /data/

EXPOSE 8082
CMD ["/app/velo-route-server", "/data/hungary-latest.osm.pbf"]
```

## License

Same as parent project.
