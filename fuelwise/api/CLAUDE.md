# Claude Code Instructions for FuelWise API

## Overview

The FuelWise API is a lightweight REST server built with mongoose (embedded HTTP library). It exposes the FuelWise optimization library via JSON endpoints.

## Quick Start

```bash
make              # Build fuelwise-api
make run          # Start on port 8080
make test         # Test with curl
```

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | Complete server implementation |
| `../vendor/mongoose/mongoose.c` | Vendored HTTP library |
| `../vendor/mongoose/mongoose.h` | mongoose headers |

## Architecture

Single-file server with:
- JSON parsing helpers (custom, no external deps)
- HTTP handlers for each endpoint
- CORS support for browser access
- Error handling with JSON responses

## Common Tasks

### Adding a new endpoint

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

3. Update startup message in `main()`.

### Modifying request parsing

JSON parsing is manual. Key functions:
- `find_json_key()` - Locate key in JSON object
- `parse_double()` - Extract number
- `parse_polyline()` - Parse [[lat,lon],...] arrays
- `parse_stations_geo()` - Parse station objects
- `parse_segments()` - Parse route segments

### Adding new JSON fields

1. Locate the relevant `parse_*` function
2. Add `find_json_key()` call for new field
3. Parse value with appropriate helper
4. Add to output struct

## Request Formats

### Solve Request (with segments)
```json
{
  "total_distance": 500,
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 8,
  "minimum_fuel": 10,
  "stations": [{"id": 1, "distance": 100, "price": 3.50}],
  "segments": [
    {"start": 0, "mpg": 6.0, "weight": 45000},
    {"start": 200, "mpg": 8.0, "weight": 30000}
  ]
}
```

### Optimize Request (full)
```json
{
  "stations": [{"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50}],
  "route": [[34.0, -118.0], [33.5, -117.0]],
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 8,
  "minimum_fuel": 10,
  "max_distance": 5,
  "segments": [...],
  "min_purchase": 10,
  "stop_cost": 5.0
}
```

## Testing

```bash
# Health check
curl http://localhost:8080/api/v1/health

# Solve with segments
curl -X POST http://localhost:8080/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{"total_distance":500,"tank_capacity":100,...}'

# Full optimization
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{"stations":[...],"route":[...],...}'
```

## Error Handling

All errors return JSON:
```json
{"error": "Error message"}
```

Status codes:
- 200: Success
- 400: Bad request (parse error)
- 404: Not found
- 405: Method not allowed
- 422: Unprocessable (infeasible problem)
- 500: Server error

## Memory Management

- All allocations freed before response
- Use `goto cleanup` pattern for error paths
- Free stations, segments, snapped arrays
- Free solution with `fw_free_solution()`

## Code Style

- 4-space indentation
- `snake_case` functions
- Static functions for internal use
- Comments for parsing logic
