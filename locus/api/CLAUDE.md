# Locus API Server - Claude Instructions

> **Quick Reference:** Use `/api-servers locus` for CLI args, env vars, and startup commands.

## Overview

The Locus API server provides RESTful geocoding endpoints using the Locus library. It runs on port 8083 by default and uses mongoose for HTTP handling.

## Quick Start

```bash
# Build
make

# Run with OSM data
./locus-geocoder ../data/monaco-latest.osm.pbf

# Run on custom port
./locus-geocoder -p 8090 ../data/monaco-latest.osm.pbf
```

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | HTTP handlers, JSON formatting |
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
  "status": "ok",
  "module": "locus",
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
  "entity_count": 2763,
  "memory_mb": 7.8,
  "bounds": {
    "min_lat": 43.7234,
    "min_lon": 7.4089,
    "max_lat": 43.7519,
    "max_lon": 7.4398
  }
}
```

### Forward Geocoding (Search)

```
GET /api/v1/search?q={query}&limit={limit}&fuzzy={0|1}
```

Parameters:
- `q` (required): Search query
- `limit` (optional): Max results (default: 10)
- `fuzzy` (optional): Enable fuzzy matching (default: 1)

Response:
```json
{
  "query": "Monte Carlo",
  "results": [
    {
      "name": "Monte Carlo",
      "lat": 43.7396,
      "lon": 7.4269,
      "class": "place",
      "score": 1.0
    }
  ],
  "total": 1,
  "time_ms": 0.012
}
```

### Autocomplete

```
GET /api/v1/autocomplete?q={prefix}&limit={limit}
```

Parameters:
- `q` (required): Prefix to complete
- `limit` (optional): Max results (default: 10)

Response:
```json
{
  "prefix": "Mon",
  "results": [
    {
      "name": "Monaco",
      "lat": 43.7384,
      "lon": 7.4246
    },
    {
      "name": "Monte Carlo",
      "lat": 43.7396,
      "lon": 7.4269
    }
  ],
  "total": 2
}
```

### Reverse Geocoding

```
GET /api/v1/reverse?lat={lat}&lon={lon}&radius={meters}
```

Parameters:
- `lat` (required): Latitude
- `lon` (required): Longitude
- `radius` (optional): Search radius in meters (default: 100)

Response:
```json
{
  "lat": 43.7384,
  "lon": 7.4246,
  "results": {
    "place": {
      "name": "Monaco",
      "lat": 43.7333,
      "lon": 7.4167,
      "class": "place"
    },
    "street": {
      "name": "Avenue de Monte-Carlo",
      "class": "street"
    }
  },
  "distance_m": 45.2
}
```

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
- 500: Server error

## Command Line Options

```
Usage: locus-geocoder [options] <pbf-file>

Options:
  -p PORT   Listen port (default: 8083)
  -h        Show help
```

## Performance

Typical response times (Monaco dataset):
- `/health`: < 1ms
- `/stats`: < 1ms
- `/search`: 1-5ms
- `/autocomplete`: 1-5ms
- `/reverse`: 1-5ms

## CORS

The server includes CORS headers for browser compatibility:
- `Access-Control-Allow-Origin: *`
- `Access-Control-Allow-Methods: GET, OPTIONS`

## Dependencies

- **liblocus.a**: Locus geocoding library
- **libshared.a**: Shared utilities
- **mongoose**: Embedded HTTP server (from vendor/)
