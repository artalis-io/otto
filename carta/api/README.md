# Carta Tile Server

A Leaflet/Google Maps compatible tile server that generates map tiles from OpenStreetMap data.

## Features

- **PNG Raster Tiles** - Standard web map tiles at configurable sizes (256/512)
- **MVT Vector Tiles** - Mapbox Vector Tile format for vector rendering
- **TileJSON** - Standard metadata endpoint for map library integration
- **Built-in Viewer** - Simple Leaflet-based tile viewer
- **React App** - Full-featured React/Leaflet application

## Quick Start

### Option 1: Native Build

```bash
# 1. Download OSM data (Hungary ~300MB)
../scripts/data-download-osm.sh hungary

# 2. Build the server
make

# 3. Run
./carta-tile-server ../data/hungary-latest.osm.pbf

# 4. Open browser at http://localhost:8081
```

### Option 2: Docker

```bash
# 1. Download OSM data
../scripts/data-download-osm.sh hungary

# 2. Build and run with docker-compose
docker-compose up --build

# 3. Open browser at http://localhost:8081
```

### Option 3: Docker (manual)

```bash
# Build from project root
docker build -t carta-tile-server -f tile-server/Dockerfile .

# Run
docker run -p 8081:8081 \
  -v $(pwd)/data:/data:ro \
  carta-tile-server /data/hungary-latest.osm.pbf
```

## API Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /` | Tile viewer |
| `GET /tiles/{z}/{x}/{y}.png` | Raster tile |
| `GET /tiles/{z}/{x}/{y}.mvt` | Vector tile |
| `GET /tiles.json` | TileJSON metadata |
| `GET /api/v1/health` | Health check |
| `GET /api/v1/stats` | PBF statistics |

## Using with Leaflet

```javascript
const map = L.map('map').setView([47.5, 19.0], 10);

L.tileLayer('http://localhost:8081/tiles/{z}/{x}/{y}.png', {
    maxZoom: 18,
    attribution: '&copy; OpenStreetMap | Carta'
}).addTo(map);
```

## Using with Google Maps

```javascript
const cartaTiles = new google.maps.ImageMapType({
    getTileUrl: (coord, zoom) =>
        `http://localhost:8081/tiles/${zoom}/${coord.x}/${coord.y}.png`,
    tileSize: new google.maps.Size(512, 512),
    name: 'Carta'
});
map.overlayMapTypes.push(cartaTiles);
```

## Configuration

### Environment Variables

```bash
TILE_PBF_PATH=/data/map.osm.pbf
TILE_PORT=8081
TILE_HOST=0.0.0.0
TILE_MIN_ZOOM=0
TILE_MAX_ZOOM=18
TILE_SIZE=512
```

### Command Line

```bash
./carta-tile-server --help

Usage: carta-tile-server [options] <pbf-file>

Options:
  -p, --port PORT      Port (default: 8081)
  -h, --host HOST      Host (default: 0.0.0.0)
  -s, --static DIR     Static files directory
  --min-zoom N         Minimum zoom level
  --max-zoom N         Maximum zoom level
  --tile-size N        PNG tile size (256 or 512)
```

## React UI Development

```bash
cd ui
npm install
npm run dev    # Development mode with hot reload
npm run build  # Production build
```

Set `VITE_TILE_SERVER` to point to the tile server:

```bash
VITE_TILE_SERVER=http://localhost:8081 npm run dev
```

## Performance

| Operation | Time |
|-----------|------|
| PBF load (300MB) | ~10-15s |
| PNG tile (512x512) | ~50-100ms |
| MVT tile | ~10-30ms |

For production, use a caching proxy (nginx, Cloudflare, etc.).

## License

MIT
