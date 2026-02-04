# Carta Tile Server - Claude Instructions

## Overview

A Leaflet/Google Maps compatible tile server that serves vector (MVT) and raster (PNG) tiles from OSM PBF files using the carta library.

## Quick Start

```bash
# Download OSM data
../scripts/download-osm.sh hungary

# Build and run
make
./carta-tile-server ../data/hungary-latest.osm.pbf

# Open browser at http://localhost:8081
```

## Production Deployment

For production, pre-build a binary index from the PBF file. The tile server auto-detects the file format and uses mmap for instant startup (<1 second vs 30+ seconds for PBF parsing).

```bash
# Step 1: Build binary index (one-time, offline)
./carta-tile-server --save-index /data/map.idx /data/map.osm.pbf

# Step 2: Run tile server from index (production)
./carta-tile-server /data/map.idx
```

### Why Use Binary Index?

| Aspect | PBF File | Binary Index |
|--------|----------|--------------|
| Startup time | 10-60s | <1s |
| Memory pattern | Peak during parse | Steady-state |
| R-tree index | Built at startup | Pre-built, mmap'd |
| Recommended for | Development | Production |

## Directory Structure

```
tile-server/
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
├── docker-compose.yml
└── CLAUDE.md           # This file
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Tile viewer (static files) |
| `/tiles/{z}/{x}/{y}.png` | GET | Raster tile (PNG) |
| `/tiles/{z}/{x}/{y}.mvt` | GET | Vector tile (MVT) |
| `/tiles.json` | GET | TileJSON metadata |
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | PBF statistics |

## Configuration

Configuration can be set via:
1. **Command line arguments**
2. **Environment variables**
3. **Configuration file** (YAML-like format)

### Command Line Options

```bash
./carta-tile-server [options] <pbf-or-index-file>

Options:
  -p, --port PORT       Port (default: 8081)
  -h, --host HOST       Host (default: 0.0.0.0)
  -s, --static DIR      Static files directory
  -c, --config FILE     Config file
  -t, --threads N       Worker threads (default: auto-detect CPU count)
  -S, --save-index PATH Save binary index to PATH after loading PBF
  --min-zoom N          Min zoom (default: 0)
  --max-zoom N          Max zoom (default: 18)
  --tile-size N         PNG size (default: 512)
  --lod none|default    LOD filtering preset
```

**Note:** The server auto-detects file format. Use `.osm.pbf` for development, `.idx` for production.

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
CARTA_THREADS=8              # Worker threads (0 = auto-detect CPU count)
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

## Docker Usage

```bash
# Build
docker build -t carta-tile-server ..

# Run with local PBF
docker run -p 8081:8081 -v $(pwd)/../data:/data \
  carta-tile-server /data/hungary-latest.osm.pbf

# Using docker-compose
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

## Integration with Leaflet

```javascript
// Standard Leaflet TileLayer
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

## Integration with Google Maps

```javascript
// Google Maps ImageMapType
const cartaTiles = new google.maps.ImageMapType({
    getTileUrl: (coord, zoom) =>
        `http://localhost:8081/tiles/${zoom}/${coord.x}/${coord.y}.png`,
    tileSize: new google.maps.Size(512, 512),
    name: 'Carta'
});
map.overlayMapTypes.push(cartaTiles);
```

## Build Commands

```bash
make          # Build tile server
make deps     # Build dependencies (carta, shared)
make debug    # Debug build
make clean    # Clean build artifacts
make run      # Run with sample data
make docker   # Build Docker image
```

## Dependencies

- **carta** - Tile generation library
- **shared** - Geo utilities
- **mongoose** - HTTP server (vendor)
- **miniz** - Compression (vendor)

## Performance Notes

- **Use binary index for production** - <1s startup vs 10-60s for PBF
- PBF loading can take 10-30 seconds for large files (100MB+)
- Binary index uses mmap - no parsing overhead, instant R-tree access
- PNG tiles are CPU-bound (~50-100ms per tile)
- MVT tiles are faster (~10-30ms per tile)
- Multi-threading enabled by default (auto-detects CPU count)
- Worker threads use thread-local render contexts for parallel tile generation
- Use nginx/CDN caching for high-traffic deployments

## Common Issues

### Port already in use
```bash
# Change port
./carta-tile-server -p 8082 map.osm.pbf
```

### PBF file not found
```bash
# Download first
../scripts/download-osm.sh hungary
```

### CORS errors in browser
The server includes CORS headers by default. If using a proxy, ensure it forwards CORS headers.
