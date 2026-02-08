# Carta Tile Server - Usage Guide

## Overview

A Leaflet/Google Maps compatible tile server that serves vector (MVT) and raster (PNG) tiles from OSM PBF files using the Carta library. Supports TileJSON metadata for auto-configuration.

## Quick Start

```bash
# Download OSM data first
../scripts/data-download-osm.sh hungary

# Build and run
cd carta/api
make
./carta-tile-server ../data/hungary-latest.osm.pbf

# Open browser at http://localhost:8081
```

## Building the Server

```bash
cd carta/api
make              # Build tile server
make deps         # Build dependencies (carta, shared)
make debug        # Debug build
make clean        # Remove build artifacts
make run          # Run with sample data
make docker       # Build Docker image
```

## Directory Structure

```
carta/api/
├── src/
│   └── main.c          # Tile server implementation
├── static/
│   └── index.html      # Simple Leaflet viewer
├── ui/                 # React/Leaflet app
│   ├── src/
│   │   ├── App.tsx     # Main component
│   │   └── App.css     # Styles
│   └── package.json
├── Makefile
├── Dockerfile
└── docker-compose.yml
```

## API Endpoints

### Tile Viewer

```bash
GET /

Returns: HTML tile viewer
```

### PNG Raster Tiles

```bash
GET /tiles/{z}/{x}/{y}.png

Example: /tiles/14/9058/5729.png
Returns: 512x512 PNG image
```

### MVT Vector Tiles

```bash
GET /tiles/{z}/{x}/{y}.mvt

Example: /tiles/14/9058/5729.mvt
Returns: Mapbox Vector Tile (protobuf)
```

### TileJSON Metadata

```bash
GET /tiles.json

Response:
{
  "tilejson": "2.2.0",
  "name": "OTTO Tiles",
  "version": "1.0.0",
  "scheme": "xyz",
  "tiles": ["http://localhost:8081/tiles/{z}/{x}/{y}.png"],
  "minzoom": 0,
  "maxzoom": 18,
  "bounds": [16.114, 45.737, 22.898, 48.585],
  "center": [19.040, 47.497, 10]
}
```

### Health Check

```bash
GET /api/v1/health

Response: {"status": "ok"}
```

### PBF Statistics

```bash
GET /api/v1/stats

Response:
{
  "nodes": 35234567,
  "ways": 4567890,
  "relations": 123456,
  "bounds": {
    "min_lat": 45.737,
    "max_lat": 48.585,
    "min_lon": 16.114,
    "max_lon": 22.898
  }
}
```

## Configuration

### Command Line Options

```bash
./carta-tile-server [options] <pbf-file>

Options:
  -p, --port PORT      Port (default: 8081)
  -h, --host HOST      Host (default: 0.0.0.0)
  -s, --static DIR     Static files directory
  -c, --config FILE    Config file
  --min-zoom N         Min zoom (default: 0)
  --max-zoom N         Max zoom (default: 18)
  --tile-size N        PNG size (default: 512)
```

### Environment Variables

```bash
TILE_PBF_PATH=/data/map.osm.pbf
TILE_PORT=8081
TILE_HOST=0.0.0.0
TILE_STATIC_DIR=./static
TILE_MIN_ZOOM=0
TILE_MAX_ZOOM=18
TILE_SIZE=512
TILE_NAME="My Tiles"
```

### Config File

```yaml
# config.yaml
pbf_path: /data/hungary-latest.osm.pbf
port: 8081
host: 0.0.0.0
static_dir: ./static
min_zoom: 0
max_zoom: 18
tile_size: 512
name: "Hungary OSM Tiles"
```

## Integration Examples

### Leaflet

```javascript
// Standard TileLayer
L.tileLayer('http://localhost:8081/tiles/{z}/{x}/{y}.png', {
    maxZoom: 18,
    attribution: '&copy; OpenStreetMap | Carta'
}).addTo(map);

// TileJSON auto-configuration
fetch('http://localhost:8081/tiles.json')
    .then(res => res.json())
    .then(tilejson => {
        map.fitBounds([
            [tilejson.bounds[1], tilejson.bounds[0]],
            [tilejson.bounds[3], tilejson.bounds[2]]
        ]);
    });
```

### Google Maps

```javascript
const cartaTiles = new google.maps.ImageMapType({
    getTileUrl: (coord, zoom) =>
        `http://localhost:8081/tiles/${zoom}/${coord.x}/${coord.y}.png`,
    tileSize: new google.maps.Size(512, 512),
    name: 'Carta'
});
map.overlayMapTypes.push(cartaTiles);
```

### MapLibre GL (Vector Tiles)

```javascript
const map = new maplibregl.Map({
    container: 'map',
    style: {
        version: 8,
        sources: {
            carta: {
                type: 'vector',
                tiles: ['http://localhost:8081/tiles/{z}/{x}/{y}.mvt'],
                maxzoom: 18
            }
        },
        layers: [
            {
                id: 'roads',
                type: 'line',
                source: 'carta',
                'source-layer': 'roads',
                paint: {'line-color': '#888'}
            }
        ]
    }
});
```

## Docker Usage

### Build and Run

```bash
# Build image
docker build -t carta-tile-server .

# Run with local PBF
docker run -p 8081:8081 -v $(pwd)/../data:/data \
  carta-tile-server /data/hungary-latest.osm.pbf
```

### Docker Compose

```yaml
# docker-compose.yml
services:
  tiles:
    build: .
    ports:
      - "8081:8081"
    volumes:
      - ../data:/data
    command: /data/hungary-latest.osm.pbf
```

```bash
docker-compose up --build
```

## React UI Development

```bash
cd ui

# Install dependencies
npm install

# Development (with hot reload)
VITE_TILE_SERVER=http://localhost:8081 npm run dev

# Build for production
npm run build
```

## Performance Notes

| Operation | Time |
|-----------|------|
| PBF loading | 10-30s (for 100MB+) |
| PNG tile (512x512) | ~50-100ms |
| MVT tile | ~10-30ms |

### Tips

- First tile requests may be slow (spatial index warm-up)
- PNG tiles are CPU-bound (rasterization)
- MVT tiles are I/O-bound (compression)
- Use nginx/CDN caching for production

## Common Issues

### Port already in use

```bash
./carta-tile-server -p 8082 map.osm.pbf
```

### PBF file not found

```bash
# Download first
../scripts/data-download-osm.sh hungary
```

### CORS errors in browser

The server includes CORS headers by default. If using a proxy, ensure it forwards CORS headers.

### Tiles appear blank

- Check coordinates are within PBF bounds
- Verify zoom level is within min/max range
- Check browser console for errors

## Dependencies

- **Carta**: Tile generation library
- **Shared**: Geo utilities
- **mongoose**: HTTP server (vendor)
- **miniz**: Compression (vendor)
