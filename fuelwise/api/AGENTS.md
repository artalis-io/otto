# Agents Guide for FuelWise REST API

## Overview

The FuelWise API is a lightweight HTTP server providing REST endpoints for refueling optimization. It uses Keel for HTTP handling and the FuelWise library for optimization.

## Directory Structure

```
api/
├── src/
│   └── main.c        # Complete server (handlers + routing)
├── Makefile
├── AGENTS.md         # This file
└── CLAUDE.md
```

## Build & Run

```bash
make              # Build fuelwise-api executable
make run          # Run on default port 8080
make run-port PORT=9000  # Run on custom port
make test         # Test endpoints with curl
```

## Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/v1/health` | Health check |
| POST | `/api/v1/filter` | Filter stations to route |
| POST | `/api/v1/solve` | Solve with pre-snapped stations |
| POST | `/api/v1/optimize` | Full pipeline |

## Code Structure

The entire API is in `src/main.c`:

```c
// JSON parsing helpers
skip_ws()           // Skip whitespace
parse_double()      // Parse number
find_json_key()     // Find key in object
parse_polyline()    // Parse [[lat,lon],...] array
parse_stations_geo() // Parse station array
parse_segments()    // Parse consumption segments
parse_solve_request() // Parse /solve request

// HTTP handlers
handle_health()     // GET /api/v1/health
handle_filter()     // POST /api/v1/filter
handle_solve()      // POST /api/v1/solve
handle_optimize()   // POST /api/v1/optimize
handle_options()    // OPTIONS (CORS preflight)

// Response helpers
send_error()        // Send JSON error
send_json()         // Send JSON response

// Event handler
handle_solve()      // Keel route handler (see also mw_* middleware)

// Entry point
main()              // Server initialization
```

## Request/Response Format

### Filter Request
```json
{
  "stations": [{"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50}],
  "route": [[34.0, -118.0], [33.5, -117.0]],
  "max_distance": 5
}
```

### Solve Request
```json
{
  "total_distance": 500,
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 8,
  "minimum_fuel": 10,
  "stations": [{"id": 1, "distance": 100, "price": 3.50}],
  "segments": [{"start": 0, "mpg": 6.0}]
}
```

### Optimize Request
```json
{
  "stations": [{"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50}],
  "route": [[34.0, -118.0], [33.5, -117.0]],
  "tank_capacity": 100,
  "current_fuel": 30,
  "consumption_mpg": 8,
  "minimum_fuel": 10
}
```

## Adding a New Endpoint

1. Add handler function:
```c
static void handle_new_endpoint(struct mg_connection *c, struct mg_http_message *hm) {
    // Parse request
    // Process
    // Send response
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

## CORS Support

All responses include:
```
Access-Control-Allow-Origin: *
```

OPTIONS requests return:
```
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
```

## Error Handling

Errors return JSON:
```json
{"error": "Error message"}
```

HTTP status codes:
- 200: Success
- 400: Bad request
- 404: Not found
- 405: Method not allowed
- 422: Unprocessable (infeasible)
- 500: Server error

## Testing

```bash
# Health check
curl http://localhost:8080/api/v1/health

# Optimization
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{"stations":[...], "route":[...]}'
```

## Configuration

- Default port: 8080 (change with `-p PORT`)
- Max request size: 10 MB
- CORS: Enabled for all origins

## Dependencies

- Keel (vendored git submodule in `../../vendor/keel/`)
- libfuelwise.a
- libralph.a
