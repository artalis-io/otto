# FuelWise API Server - Usage Guide

## Overview

The FuelWise API is a lightweight REST server built with mongoose (embedded HTTP library). It exposes the FuelWise optimization library via JSON endpoints for refueling optimization.

## Quick Start

```bash
# Build and run
cd fuelwise/api
make
make run          # Start on port 8080

# Test
curl http://localhost:8080/api/v1/health
```

## Building the API

```bash
cd fuelwise/api
make              # Build fuelwise-api
make run          # Start server on port 8080
make test         # Run integration tests
make debug        # Debug build
make clean        # Remove artifacts
```

## Directory Structure

```
fuelwise/api/
├── src/
│   └── main.c            # Complete server implementation
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

### Filter Stations

Filter stations to those near a route polyline.

```bash
POST /api/v1/filter
Content-Type: application/json

{
  "stations": [
    {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
    {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.45}
  ],
  "route": [[34.05, -118.24], [33.45, -112.07]],
  "max_distance": 5.0
}

Response:
{
  "stations": [
    {"id": 1, "distance": 0.0, "price": 3.89},
    {"id": 2, "distance": 456.2, "price": 3.45}
  ]
}
```

### Solve Refueling Problem

Solve with pre-computed station distances.

```bash
POST /api/v1/solve
Content-Type: application/json

{
  "total_distance": 500,
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 6.5,
  "minimum_fuel": 10,
  "stations": [
    {"id": 1, "distance": 100, "price": 3.50},
    {"id": 2, "distance": 250, "price": 3.20}
  ]
}

Response:
{
  "status": "optimal",
  "total_cost": 185.50,
  "purchases": [
    {"station_id": 1, "gallons": 25.5},
    {"station_id": 2, "gallons": 40.2}
  ]
}
```

### Optimize (Full Pipeline)

Combined filter + solve in one request.

```bash
POST /api/v1/optimize
Content-Type: application/json

{
  "stations": [
    {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
    {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.45}
  ],
  "route": [[34.05, -118.24], [33.45, -112.07]],
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 6.5,
  "minimum_fuel": 10,
  "max_distance": 5.0
}

Response:
{
  "status": "optimal",
  "total_cost": 156.20,
  "total_distance": 456.2,
  "purchases": [
    {"station_id": 2, "gallons": 45.3, "distance": 250.0}
  ]
}
```

## Request Options

### Solve/Optimize Request Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `total_distance` | number | Yes* | Total route distance (miles) |
| `tank_capacity` | number | Yes | Tank capacity (gallons) |
| `current_fuel` | number | Yes | Starting fuel (gallons) |
| `consumption_mpg` | number | Yes** | Fuel consumption (mpg) |
| `minimum_fuel` | number | No | Minimum fuel level (default: 0) |
| `stations` | array | Yes | Array of stations |
| `route` | array | Yes*** | Route polyline [[lat, lon], ...] |
| `max_distance` | number | No | Max distance from route (miles, default: 5) |
| `segments` | array | No | Piecewise consumption segments |
| `min_purchase` | number | No | Minimum purchase if stopping |
| `stop_cost` | number | No | Fixed cost per stop (enables MILP) |

\* Required for `/solve`, computed from route for `/optimize`
\** Required unless `segments` provided
\*** Required for `/filter` and `/optimize`

### Segment Object (Piecewise Consumption)

```json
{
  "segments": [
    {"start": 0, "mpg": 5.5, "weight": 45000},
    {"start": 200, "mpg": 7.0, "weight": 30000}
  ]
}
```

## Error Handling

All errors return JSON:

```json
{"error": "Error message"}
```

### Status Codes

| Code | Description |
|------|-------------|
| 200 | Success |
| 400 | Bad request (parse error) |
| 404 | Not found |
| 405 | Method not allowed |
| 422 | Unprocessable (infeasible problem) |
| 500 | Server error |

## Examples

### Basic Optimization

```bash
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{
    "stations": [
      {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
      {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.45}
    ],
    "route": [[34.05, -118.24], [33.45, -112.07]],
    "tank_capacity": 300,
    "current_fuel": 50,
    "consumption_mpg": 6.5
  }'
```

### With Piecewise Consumption

```bash
curl -X POST http://localhost:8080/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{
    "total_distance": 500,
    "tank_capacity": 100,
    "current_fuel": 30,
    "minimum_fuel": 10,
    "stations": [
      {"id": 1, "distance": 100, "price": 3.50},
      {"id": 2, "distance": 300, "price": 3.20}
    ],
    "segments": [
      {"start": 0, "mpg": 5.5, "weight": 45000},
      {"start": 200, "mpg": 7.0, "weight": 30000}
    ]
  }'
```

### With Stop Costs (MILP)

```bash
curl -X POST http://localhost:8080/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{
    "total_distance": 500,
    "tank_capacity": 100,
    "current_fuel": 30,
    "consumption_mpg": 6.5,
    "stations": [
      {"id": 1, "distance": 100, "price": 3.50},
      {"id": 2, "distance": 250, "price": 3.20}
    ],
    "min_purchase": 10,
    "stop_cost": 5.0
  }'
```

## Development

### Adding a New Endpoint

1. Create handler function:
```c
static void handle_new_endpoint(struct mg_connection *c, struct mg_http_message *hm) {
    // Parse JSON body from hm->body
    // Call FuelWise functions
    // Send JSON response
}
```

2. Add route in `ev_handler()`:
```c
} else if (mg_match(hm->uri, mg_str("/api/v1/new_endpoint"), NULL)) {
    if (mg_match(hm->method, mg_str("POST"), NULL)) {
        handle_new_endpoint(c, hm);
    } else {
        send_error(c, 405, "Method not allowed");
    }
}
```

### JSON Parsing Helpers

- `find_json_key()` - Locate key in JSON object
- `parse_double()` - Extract number
- `parse_polyline()` - Parse [[lat,lon],...] arrays
- `parse_stations_geo()` - Parse station objects
- `parse_segments()` - Parse route segments

## Dependencies

- **FuelWise**: Refueling library
- **Ralph**: LP/MIP solver
- **mongoose**: HTTP server (vendor)
