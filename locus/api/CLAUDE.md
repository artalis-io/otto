# Locus API Server - Claude Instructions

> **Quick Reference:** Use `/api-servers locus` for CLI args, env vars, and startup commands.

## Overview

The Locus API server provides RESTful geocoding endpoints using the Locus library. It runs on port 8083 by default and uses mongoose for HTTP handling.

Features:
- Rate limiting (per-IP token bucket)
- Work queue with backpressure
- Adaptive capacity tuning
- Socket timeout protection
- Structured logging
- Distributed tracing
- Prometheus metrics

## Quick Start

```bash
# Build
make

# Run with OSM data
./locus-geocoder ../data/monaco-latest.osm.pbf

# Run on custom port
./locus-geocoder -p 8090 ../data/monaco-latest.osm.pbf

# Run with saved index (fast startup)
./locus-geocoder -S monaco.idx ../data/monaco-latest.osm.pbf  # Build and save
./locus-geocoder monaco.idx                                    # Load from index
```

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | HTTP handlers, work queue, rate limiting |
| `Makefile` | Build configuration |

## Build Targets

```bash
make          # Build server
make run      # Run server (requires data)
make test     # Test endpoints (server must be running)
make clean    # Remove build artifacts
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
  "service": "locus",
  "version": "0.1.0"
}
```

### Statistics

```
GET /api/v1/stats
```

Response:
```json
{
  "entities": 2763,
  "memory_mb": 7.8,
  "bounds": {
    "min_lat": 43.7234,
    "min_lon": 7.4089,
    "max_lat": 43.7519,
    "max_lon": 7.4398
  },
  "rate_limit": {
    "enabled": true,
    "rps": 50.0,
    "burst": 100,
    "allowed": 12345,
    "denied": 42
  },
  "work_queue": {
    "enabled": true,
    "depth": 5,
    "capacity": 128,
    "pushed": 1000,
    "popped": 995,
    "dropped": 2,
    "expired": 3
  },
  "adaptive": {
    "enabled": true,
    "samples": 500,
    "response_ms": {"p50": 0.5, "p90": 1.2, "p99": 2.5}
  }
}
```

### Forward Geocoding (Search)

```
GET /api/v1/search?q={query}&limit={limit}
```

Parameters:
- `q` (required): Search query
- `limit` (optional): Max results (default: 10, max: 100)

Response:
```json
{
  "query": "Monte Carlo",
  "total": 1,
  "took_ms": 0.12,
  "results": [
    {
      "osm_id": 12345,
      "osm_type": "node",
      "name": "Monte Carlo",
      "class": "place",
      "lat": 43.7396,
      "lon": 7.4269,
      "score": 1.0
    }
  ]
}
```

### Autocomplete

```
GET /api/v1/autocomplete?q={prefix}&limit={limit}
```

Parameters:
- `q` (required): Prefix to complete
- `limit` (optional): Max results (default: 10, max: 20)

Response:
```json
[
  "Monaco",
  "Monte Carlo"
]
```

### Reverse Geocoding

```
GET /api/v1/reverse?lat={lat}&lon={lon}
```

Parameters:
- `lat` (required): Latitude
- `lon` (required): Longitude

Response:
```json
{
  "lat": 43.7384,
  "lon": 7.4246,
  "display_name": "Avenue de Monte-Carlo, Monaco",
  "distance_m": 45.2,
  "place": "Monaco",
  "street": "Avenue de Monte-Carlo"
}
```

### Prometheus Metrics

```
GET /metrics
```

Returns Prometheus-format metrics for monitoring.

## Error Responses

All endpoints return errors in this format:

```json
{
  "error": "Error message here"
}
```

HTTP status codes:
- 200: Success
- 400: Bad request (missing/invalid parameters)
- 429: Rate limit exceeded
- 503: Server busy (queue full)
- 504: Request timeout

## Command Line Options

```
Usage: locus-geocoder [options] <pbf-or-idx-file>

Common options:
  -p, --port PORT           Listen port (default: 8083)
  -h, --host HOST           Bind address (default: 0.0.0.0)
  -t, --threads N           Worker threads (default: auto)
  --rate-limit-rps N        Rate limit (requests/sec, default: 50)
  --rate-limit-burst N      Burst capacity (default: 100)
  --rate-limit-off          Disable rate limiting
  --queue-depth N           Work queue depth (default: 128)
  --queue-timeout N         Queue timeout seconds (default: 5)
  --queue-off               Disable work queue
  --adaptive                Enable adaptive capacity
  -v, --verbose             Increase verbosity

Locus-specific options:
  -S, --save PATH           Save index to binary file after building
  --build-only              Exit after building/saving index (no HTTP server)
  --workers N               Geocode worker threads (default: auto)
```

## Environment Variables

```bash
# Network
LOCUS_PORT=8083              # Listen port
LOCUS_HOST=0.0.0.0           # Bind address
LOCUS_THREADS=0              # Worker threads (0 = auto)

# Data
LOCUS_DATA_FILE=             # Path to data file (PBF or index)
LOCUS_NUM_WORKERS=0          # Geocode workers (0 = auto)

# Rate limiting
LOCUS_RATE_LIMIT_ENABLED=1   # Enable rate limiting
LOCUS_RATE_LIMIT_RPS=50      # Requests/sec per IP
LOCUS_RATE_LIMIT_BURST=100   # Burst capacity

# Work queue (backpressure)
LOCUS_WORK_QUEUE_ENABLED=1   # Enable work queue
LOCUS_WORK_QUEUE_DEPTH=128   # Max pending requests
LOCUS_WORK_QUEUE_TIMEOUT=5   # Request timeout seconds

# Adaptive capacity
LOCUS_ADAPTIVE_ENABLED=0     # Enable adaptive capacity
LOCUS_TARGET_UTILIZATION=0.7 # Target utilization 0.0-1.0
LOCUS_CLIENT_TIMEOUT=10000   # Client timeout in ms

# CORS configuration
LOCUS_CORS_ORIGINS=          # Comma-separated allowed origins
LOCUS_CORS_METHODS=          # Allowed HTTP methods
LOCUS_CORS_HEADERS=          # Allowed request headers
LOCUS_CORS_CREDENTIALS=0     # Allow credentials

# Logging
SH_LOG_LEVEL=info            # Log level: debug, info, warn, error
SH_LOG_FORMAT=text           # Log format: text, json
```

## Performance

Typical response times (Monaco dataset, 2763 entities):
- `/health`: < 1ms
- `/stats`: < 1ms
- `/search`: 5-11 µs/query
- `/autocomplete`: 5-19 µs/query
- `/reverse`: 23-24 µs/query

## CORS

By default, allows all origins (`Access-Control-Allow-Origin: *`).

For production, restrict to specific origins:
```bash
export LOCUS_CORS_ORIGINS="https://app.example.com,https://staging.example.com"
```

## Dependencies

- **liblocus.a**: Locus geocoding library
- **libshared.a**: Shared utilities (rate limiting, work queue, logging)
- **mongoose**: Embedded HTTP server (from vendor/)
