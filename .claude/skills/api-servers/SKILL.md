---
name: api-servers
description: Quick reference for starting OTTO API servers (Carta, Velo, Locus, FuelWise) with CLI arguments and environment variables.
user-invocable: true
---

# OTTO API Servers Quick Reference

Start and configure the OTTO API servers: Carta (tiles), Velo (routing), Locus (geocoding), and FuelWise (refueling optimization).

## Quick Start

```bash
# Build all APIs
make carta-api velo-api locus-api fuelwise-api

# Start with default settings
./carta/api/carta-tile-server data/hungary-latest.osm.pbf    # :8081
./velo/api/velo-route-server data/hungary-latest.osm.pbf     # :8082
./locus/api/locus-geocoder data/hungary-latest.osm.pbf       # :8083
./fuelwise/api/fuelwise-api                                   # :8080
```

---

## Server Summary

| Server | Default Port | Data File | Health Endpoint |
|--------|--------------|-----------|-----------------|
| **Carta** (tiles) | 8081 | `.osm.pbf` or `.idx` | `/api/v1/health` |
| **Velo** (routing) | 8082 | `.osm.pbf` or `.vlg` | `/api/v1/health` |
| **Locus** (geocoding) | 8083 | `.osm.pbf` | `/api/v1/health` |
| **FuelWise** (optimization) | 8080 | None | `/api/v1/health` |

---

## Common CLI Arguments (All Servers)

All servers use `sh_args` for consistent argument parsing:

```bash
# Network
-p, --port PORT           Listen port
-h, --host HOST           Bind address (default: 0.0.0.0)
-t, --threads N           Worker threads (0 = auto-detect CPU count)
-s, --static DIR          Static files directory

# Rate Limiting
--rate-limit-rps N        Requests per second per IP (default: 10)
--rate-limit-burst N      Burst capacity in tokens (default: 100)
--rate-limit-off          Disable rate limiting

# Work Queue
--queue-depth N           Max pending requests (default: 100-256)
--queue-timeout N         Request timeout in seconds (default: 5-10)
--queue-off               Disable work queue

# Adaptive Capacity
--adaptive                Enable adaptive capacity (self-tuning)
--utilization N           Target utilization 0.0-1.0 (default: 0.7)
--client-timeout N        Client timeout in ms (default: 10000)

# Logging
-v, --verbose             Increase verbosity
-q, --quiet               Suppress non-error output
--help                    Show help
```

---

## Carta Tile Server

Serves PNG and MVT tiles from OSM data.

### Start Commands

```bash
# Development (parses PBF on startup, 10-60s)
./carta/api/carta-tile-server data/hungary-latest.osm.pbf

# Production (instant startup with pre-built index)
./carta/api/carta-tile-server data/hungary.idx

# Build index for production
./carta/api/carta-tile-server --save-index data/hungary.idx data/hungary-latest.osm.pbf
```

### Carta-Specific Arguments

```bash
-S, --save-index PATH     Save binary index after loading PBF
--min-zoom N              Minimum zoom level (default: 0)
--max-zoom N              Maximum zoom level (default: 18)
--tile-size N             PNG tile size in pixels (default: 512)
--lod none|default        Level-of-detail filtering preset
--burst-tiles N           Tiles in initial map view (default: 25)
```

### Environment Variables (CARTA_ prefix)

```bash
# Basic
CARTA_PORT=8081
CARTA_HOST=0.0.0.0
CARTA_THREADS=8
CARTA_STATIC_DIR=./static
CARTA_DATA_FILE=/data/map.osm.pbf

# Carta-specific
CARTA_MIN_ZOOM=0
CARTA_MAX_ZOOM=18
CARTA_TILE_SIZE=512
CARTA_NAME="My Tiles"
CARTA_LOD=default
CARTA_RENDER_WORKERS=8

# Rate Limiting
CARTA_RATE_LIMIT_ENABLED=1
CARTA_RATE_LIMIT_RPS=10
CARTA_RATE_LIMIT_BURST=100

# Work Queue
CARTA_WORK_QUEUE_ENABLED=1
CARTA_WORK_QUEUE_DEPTH=256
CARTA_WORK_QUEUE_TIMEOUT=5

# Adaptive Capacity
CARTA_ADAPTIVE_ENABLED=0
CARTA_TARGET_UTILIZATION=0.7
CARTA_CLIENT_TIMEOUT=10000
CARTA_BURST_TILES=25
CARTA_ADAPTIVE_WINDOW=1000
CARTA_ADAPTIVE_INTERVAL=1000

# CORS
CARTA_CORS_ORIGINS=                    # Empty = allow all (*)
CARTA_CORS_CREDENTIALS=0
```

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/tiles/{z}/{x}/{y}.png` | GET | Raster tile |
| `/tiles/{z}/{x}/{y}.mvt` | GET | Vector tile |
| `/tiles.json` | GET | TileJSON metadata |
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | PBF/index statistics |

---

## Velo Route Server

Computes routes between coordinates with vehicle profiles.

### Start Commands

```bash
# Basic (parses PBF + builds landmarks, ~30s)
./velo/api/velo-route-server data/hungary-latest.osm.pbf

# Fast startup with pre-built graph
./velo/api/velo-route-server data/hungary.vlg

# Custom landmarks
./velo/api/velo-route-server --landmarks 32 data/hungary-latest.osm.pbf

# Disable landmarks (slower queries, faster startup)
./velo/api/velo-route-server --no-landmarks data/hungary-latest.osm.pbf
```

### Velo-Specific Arguments

```bash
--landmarks N             Number of landmarks for ALT (default: 16)
--no-landmarks            Disable landmark preprocessing
-c, --config FILE         Config file path
```

### Environment Variables (VELO_ prefix)

```bash
# Basic
VELO_PORT=8082
VELO_HOST=0.0.0.0
VELO_THREADS=8

# Velo-specific
ROUTE_GRAPH_PATH=/data/map.osm.pbf
ROUTE_LANDMARKS=1
ROUTE_LANDMARK_COUNT=16

# Rate Limiting
VELO_RATE_LIMIT_ENABLED=1
VELO_RATE_LIMIT_RPS=10
VELO_RATE_LIMIT_BURST=100

# Work Queue
VELO_WORK_QUEUE_ENABLED=1
VELO_WORK_QUEUE_DEPTH=100
VELO_WORK_QUEUE_TIMEOUT=10

# Adaptive Capacity
VELO_ADAPTIVE_ENABLED=0
VELO_TARGET_UTILIZATION=0.7
VELO_CLIENT_TIMEOUT=10000
```

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/route` | GET/POST | Calculate route |
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Graph statistics |

### Route Parameters

```
?from=LAT,LON             Origin (required)
&to=LAT,LON               Destination (required)
&profile=car|truck|bike|foot  Vehicle type (default: car)
&mode=fastest|shortest    Optimization (default: fastest)
&geometry=true|false      Include polyline (default: true)
```

### Example

```bash
curl 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=truck&mode=fastest'
```

---

## Locus Geocoding Server

Forward/reverse geocoding and autocomplete.

### Start Commands

```bash
# Basic
./locus/api/locus-geocoder data/monaco-latest.osm.pbf

# Custom port
./locus/api/locus-geocoder -p 8090 data/hungary-latest.osm.pbf
```

### Locus-Specific Arguments

```bash
-p PORT                   Listen port (default: 8083)
-h                        Show help
```

### Environment Variables (LOCUS_ prefix)

```bash
LOCUS_PORT=8083
LOCUS_HOST=0.0.0.0
LOCUS_THREADS=8

# Rate Limiting
LOCUS_RATE_LIMIT_ENABLED=1
LOCUS_RATE_LIMIT_RPS=20
LOCUS_RATE_LIMIT_BURST=100
```

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/search` | GET | Forward geocoding |
| `/api/v1/autocomplete` | GET | Prefix autocomplete |
| `/api/v1/reverse` | GET | Reverse geocoding |
| `/api/v1/health` | GET | Health check |
| `/api/v1/stats` | GET | Index statistics |

### Search Parameters

```
# Forward search
/api/v1/search?q=Budapest&limit=10&fuzzy=1

# Autocomplete
/api/v1/autocomplete?q=Bud&limit=10

# Reverse
/api/v1/reverse?lat=47.4979&lon=19.0402&radius=100
```

---

## FuelWise API Server

Refueling optimization (no data file required).

### Start Commands

```bash
# Basic
./fuelwise/api/fuelwise-api

# Custom port with rate limiting
./fuelwise/api/fuelwise-api -p 9000 --rate-limit-rps 5

# Production with adaptive capacity
./fuelwise/api/fuelwise-api --adaptive --utilization 0.8
```

### Environment Variables (FUELWISE_ prefix)

```bash
# Basic
FUELWISE_PORT=8080
FUELWISE_HOST=0.0.0.0
FUELWISE_THREADS=8

# Rate Limiting
FUELWISE_RATE_LIMIT_ENABLED=1
FUELWISE_RATE_LIMIT_RPS=10
FUELWISE_RATE_LIMIT_BURST=100

# Work Queue
FUELWISE_WORK_QUEUE_ENABLED=1
FUELWISE_WORK_QUEUE_DEPTH=100
FUELWISE_WORK_QUEUE_TIMEOUT=10
```

### Endpoints

| Endpoint | Method | Queued | Description |
|----------|--------|--------|-------------|
| `/api/v1/solve` | POST | Yes | Solve refueling LP/MILP |
| `/api/v1/filter` | POST | Yes | Filter stations to route |
| `/api/v1/optimize` | POST | Yes | Full optimization pipeline |
| `/api/v1/health` | GET | No | Health check |
| `/api/v1/stats` | GET | No | Queue/rate limit stats |

---

## Common Configurations

### Development (permissive)

```bash
# Disable rate limiting and work queue for testing
./carta/api/carta-tile-server --rate-limit-off --queue-off data/map.osm.pbf
```

### Production (hardened)

```bash
# Enable all protection with adaptive tuning
export CARTA_RATE_LIMIT_ENABLED=1
export CARTA_RATE_LIMIT_RPS=10
export CARTA_RATE_LIMIT_BURST=50
export CARTA_WORK_QUEUE_ENABLED=1
export CARTA_WORK_QUEUE_DEPTH=256
export CARTA_ADAPTIVE_ENABLED=1
export CARTA_TARGET_UTILIZATION=0.7
./carta/api/carta-tile-server data/map.idx
```

### Docker

```bash
# Carta with volume mount
docker run -p 8081:8081 -v $(pwd)/data:/data \
  -e CARTA_PORT=8081 \
  carta-tile-server /data/map.idx

# All servers via docker-compose
docker-compose --profile all-apis up
```

### CORS Configuration

```bash
# Allow specific origins (production)
export CARTA_CORS_ORIGINS="https://app.example.com,https://staging.example.com"

# Allow all origins (development, default)
export CARTA_CORS_ORIGINS=""
```

---

## Troubleshooting

### Port already in use

```bash
# Check what's using the port
lsof -i :8081

# Use different port
./carta/api/carta-tile-server -p 8082 data/map.osm.pbf
```

### Missing data file

```bash
# Download OSM data
./scripts/download-osm.sh hungary
./scripts/download-osm.sh monaco  # Small, for testing
./scripts/download-osm.sh list    # Show available regions
```

### Slow startup

```bash
# Carta: Use binary index for instant startup
./carta/api/carta-tile-server --save-index data/map.idx data/map.osm.pbf
./carta/api/carta-tile-server data/map.idx  # <1s startup

# Velo: Use binary graph
./velo/api/velo-route-server data/map.vlg   # ~100ms vs ~8s for PBF
```

### Out of memory

```bash
# Set memory limit (Carta)
export CARTA_MEMORY_LIMIT=4G
./carta/api/carta-tile-server data/large.osm.pbf
```

### Health check

```bash
# Verify server is running
curl http://localhost:8081/api/v1/health
curl http://localhost:8082/api/v1/health
curl http://localhost:8083/api/v1/health
curl http://localhost:8080/api/v1/health
```

---

## Make Targets

```bash
# Build
make carta-api          # Build Carta tile server
make velo-api           # Build Velo route server
make locus-api          # Build Locus geocoder
make fuelwise-api       # Build FuelWise API

# Run (shows usage or starts with sample data)
make run-carta-api
make run-velo-api
make run-locus-api
make run-fuelwise-api

# Test
make test-carta-api     # API integration tests
make test-velo-api
make test-locus-api
make test-fuelwise-api
```
