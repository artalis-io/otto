# Claude Code Instructions for FuelWise API

> **Quick Reference:** Use `/api-run fuelwise` for CLI args, env vars, and startup commands.

## Overview

The FuelWise API is a lightweight REST server built with Keel (embedded HTTP library, MIT). It exposes the FuelWise optimization library via JSON endpoints with production-grade features including rate limiting, work queue for CPU-intensive operations, and configurable through CLI args and environment variables.

## Quick Start

```bash
make              # Build fuelwise-api
make run          # Start on port 8080
make test         # Run full API test suite (20 tests)
make test-quick   # Quick smoke test
```

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | Complete server implementation |
| `test_api.sh` | API test suite |
| `../../vendor/keel/` | Vendored HTTP library (git submodule) |
| `../../shared/include/sh_ratelimit.h` | Rate limiting |
| `../../shared/include/sh_workqueue.h` | Work queue |
| `../../shared/include/sh_args.h` | CLI/env argument parsing |

## Architecture

Single-file server with:
- Rate limiting (token bucket per IP, supports IPv4/IPv6)
- Work queue for CPU-intensive endpoints (solve, filter, optimize)
- Configurable via CLI args or environment variables (sh_args)
- JSON parsing helpers (custom, no external deps)
- HTTP handlers for each endpoint
- CORS support for browser access
- Error handling with JSON responses

## API Endpoints

| Endpoint | Method | Queue | Description |
|----------|--------|-------|-------------|
| `/api/v1/health` | GET | No | Health check (bypasses queue/rate limit) |
| `/api/v1/stats` | GET | No | Server statistics (bypasses queue/rate limit) |
| `/api/v1/solve` | POST | Yes | Solve refueling problem |
| `/api/v1/filter` | POST | Yes | Filter stations to route |
| `/api/v1/optimize` | POST | Yes | Full optimization pipeline |

## Configuration

### Command Line Options

```bash
./fuelwise-api [options]

Network:
  -p, --port PORT           Listen port (default: 8080)
  -h, --host HOST           Bind address (default: 0.0.0.0)
  -t, --threads N           Worker threads (default: auto)
  -s, --static DIR          Static files directory

Rate Limiting:
  --rate-limit-rps N        Requests per second per IP (default: 10)
  --rate-limit-burst N      Burst capacity (default: 100)
  --rate-limit-off          Disable rate limiting

Work Queue:
  --queue-depth N           Max pending requests (default: 100)
  --queue-timeout N         Request timeout in seconds (default: 10)
  --queue-off               Disable work queue

Adaptive Capacity:
  --adaptive                Enable adaptive capacity
  --utilization N           Target utilization 0.0-1.0 (default: 0.7)
  --client-timeout N        Client timeout in ms (default: 10000)

Logging:
  -v, --verbose             Increase verbosity
  -q, --quiet               Suppress non-error output
  --help                    Show help
```

### Environment Variables

```bash
FUELWISE_PORT=8080
FUELWISE_HOST=0.0.0.0
FUELWISE_THREADS=8
FUELWISE_RATE_LIMIT_ENABLED=1
FUELWISE_RATE_LIMIT_RPS=10
FUELWISE_RATE_LIMIT_BURST=100
FUELWISE_WORK_QUEUE_ENABLED=1
FUELWISE_WORK_QUEUE_DEPTH=100
FUELWISE_WORK_QUEUE_TIMEOUT=10
```

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

### Filter Request
```json
{
  "stations": [{"id": 1, "lat": 34.0, "lon": -118.0, "price": 3.50}],
  "route": [[34.0, -118.0], [33.5, -117.0]],
  "max_distance": 5
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

## Stats Endpoint

Returns work queue and rate limit statistics:

```json
{
  "service": "fuelwise-api",
  "version": "1.0.0",
  "work_queue": {
    "enabled": true,
    "depth": 0,
    "capacity": 100,
    "pushed": 15,
    "popped": 15,
    "dropped": 0,
    "expired": 0,
    "timeout_sec": 10.0
  },
  "rate_limit": {
    "enabled": true,
    "rps": 10.0,
    "burst": 100.0,
    "allowed": 150,
    "denied": 12,
    "active_entries": 5,
    "evictions": 0
  }
}
```

## Testing

```bash
# Full test suite (20 tests)
make test

# Quick smoke test
make test-quick

# Rate limiting tests only
make test-ratelimit

# Work queue tests only
make test-queue

# Manual testing
curl http://localhost:8080/api/v1/health
curl http://localhost:8080/api/v1/stats
curl -X POST http://localhost:8080/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{"total_distance":500,"tank_capacity":100,...}'
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
- 429: Too Many Requests (rate limited)
- 500: Server error
- 503: Service Unavailable (queue full)
- 504: Gateway Timeout (request expired in queue)

## Common Tasks

### Adding a new endpoint

Endpoints are added in `fuelwise/src/fw_api.c`, not in the server. The server
is transport only: it hands `fw_api_handle()` a whole `ShApiRequest` and writes
back whatever `ShApiResponse` it gets. Nothing about a route reaches
`api/src/main.c`, which is why the same endpoint works from WASM and from an
embedded caller without being written twice.

1. Write the handler beside the others in `fw_api.c`. It takes the parsed
   request and fills the response; it does not know what a socket is:

```c
static int handle_new_endpoint(FWAPIContext *ctx,
                               const ShApiRequest *req,
                               ShApiResponse *resp)
{
    /* Parse req->body / req->body_len, call the FuelWise core, then either
     * sh_api_response_error(resp, 400, "...") or fill resp->body,
     * resp->body_len, resp->status_code and resp->content_type. */
}
```

2. Route to it in `fw_api_handle()`, in the same chain as the existing paths:

```c
else if (strcmp(req->path, "/api/v1/new_endpoint") == 0) {
    return handle_new_endpoint(ctx, req, resp);
}
```

3. Add a case to `fuelwise/api/test_api.sh` and, if the endpoint parses
   untrusted input, a seed to `fuelwise/tests/fuzz/corpus_api/`. The fuzzer
   drives `fw_api_handle()` directly, so a new route is covered as soon as it
   is routed -- two control bytes at the front of a seed pick the path and the
   method.

The queue and the rate limiter sit in front of all of this in
`sh_http_async_dispatch()` and need no per-endpoint change: a new route is
queued, rate-limited and deadlined exactly like the existing ones.

### Modifying rate limits

Rate limits use the shared library token bucket implementation:
- `sh_ratelimit_create()` - Create rate limiter
- `sh_ratelimit_check()` - Check if request allowed
- `sh_ratelimit_stats()` - Get statistics

### Modifying work queue

Work queue uses the shared library bounded queue:
- `sh_workqueue_create()` - Create queue
- `sh_workqueue_push()` - Add work item
- `sh_workqueue_pop_timeout()` - Get work item with timeout
- `sh_workqueue_item_expired()` - Check if item is stale

## Memory Management

- All allocations freed before response
- Work items free their own request body copies
- Use `goto cleanup` pattern for error paths
- Free stations, segments, snapped arrays
- Free solution with `fw_free_solution()`

## Code Style

- 4-space indentation
- `snake_case` functions
- Static functions for internal use
- Comments for parsing logic
- Thread-safe signal handling with `volatile sig_atomic_t`
