# Carta Tile Server - Claude Instructions

> **Quick Reference:** Use `/api-run carta` for CLI args, env vars, and startup commands.

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
  --render-preset P     Render quality: default, fast, quality
  --adaptive            Enable adaptive capacity (auto-tune rate limits)
  --utilization N       Target utilization 0.0-1.0 (default: 0.7)
  --client-timeout N    Client timeout in ms (default: 10000)
  --burst-tiles N       Tiles in initial view for burst sizing (default: 25)
```

**Note:** The server auto-detects file format. Use `.osm.pbf` for development, `.idx` for production.

### Environment Variables

```bash
# Basic configuration
TILE_PBF_PATH=/data/map.osm.pbf      # Also: CARTA_DATA_FILE
CARTA_PORT=8081                       # Listen port (default: 8081)
CARTA_HOST=0.0.0.0                    # Bind address (default: 0.0.0.0)
CARTA_STATIC_DIR=./static             # Static files directory
CARTA_THREADS=8                       # Worker threads (0 = auto-detect)

# Carta-specific
CARTA_MIN_ZOOM=0                      # Minimum zoom level
CARTA_MAX_ZOOM=18                     # Maximum zoom level
CARTA_TILE_SIZE=512                   # PNG tile size
CARTA_NAME="My Tiles"                 # Server name in TileJSON
CARTA_LOD=default                     # LOD preset (none, default, detailed, minimal)
CARTA_RENDER_PRESET=default           # Render quality (default, fast, quality)
CARTA_RENDER_WORKERS=8                # Render worker threads (0 = auto)

# Rate limiting
CARTA_RATE_LIMIT_ENABLED=1            # Enable rate limiting (default: 1)
CARTA_RATE_LIMIT_RPS=10               # Requests per second per IP (default: 10)
CARTA_RATE_LIMIT_BURST=100            # Burst capacity (default: 100)

# Work queue (backpressure)
CARTA_WORK_QUEUE_ENABLED=1            # Enable work queue (default: 1)
CARTA_WORK_QUEUE_DEPTH=256            # Max pending requests (default: 256)
CARTA_WORK_QUEUE_TIMEOUT=5            # Request timeout in seconds (default: 5)

# Adaptive capacity (self-tuning based on measured response times)
CARTA_ADAPTIVE_ENABLED=0              # Enable adaptive capacity (default: 0)
CARTA_TARGET_UTILIZATION=0.7          # Target utilization 0.0-1.0 (default: 0.7)
CARTA_CLIENT_TIMEOUT=10000            # Client timeout in ms (default: 10000)
CARTA_BURST_TILES=25                  # Tiles in initial view (default: 25)
CARTA_ADAPTIVE_WINDOW=1000            # Sample window for percentiles (default: 1000)
CARTA_ADAPTIVE_INTERVAL=1000          # Recalc interval in requests (default: 1000)

# CORS configuration
CARTA_CORS_ORIGINS=                   # Comma-separated allowed origins (empty = allow all with *)
CARTA_CORS_METHODS="GET, POST, OPTIONS"  # Allowed HTTP methods
CARTA_CORS_HEADERS="Content-Type, Authorization"  # Allowed request headers
CARTA_CORS_CREDENTIALS=0              # Allow credentials (default: 0)
```

### CORS Configuration

By default, the server allows all origins (`Access-Control-Allow-Origin: *`). For production, you can restrict to specific origins:

```bash
# Allow only specific origins
export CARTA_CORS_ORIGINS="https://app.example.com,https://staging.example.com"

# The server will:
# - Return the requesting origin if it matches the whitelist
# - Return no CORS headers if the origin doesn't match (browser blocks request)
# - Work correctly with preflight (OPTIONS) requests
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
- **Keel** - HTTP server (vendor submodule, MIT)
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
