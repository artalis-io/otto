# Ralph Solver Server - Claude Instructions

> **Quick Reference:** Use `/api-run ralph` for CLI args, env vars, and startup commands.

## Overview

HTTP API server for the Ralph LP/MIP solver. Designed for WASM demos and lightweight deployments. Stateless - each request is independent.

## Quick Start

```bash
# Build and run
make
./ralph-solver-server

# Test
curl http://localhost:8084/api/v1/health
curl -X POST http://localhost:8084/api/v1/solve \
  -H "Content-Type: application/json" \
  -d '{"format":"lp","problem":"max: 2x + 3y\nsubject to\nc1: x + y <= 10\nend"}'
```

## Directory Structure

```
api/
├── src/
│   └── main.c        # HTTP server (Keel wrapper)
├── Makefile
└── CLAUDE.md         # This file
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/formats` | GET | Supported input formats |
| `/api/v1/solve` | POST | Solve LP/MIP problem |

## Solve Request Format

```json
{
  "format": "lp",
  "problem": "max: 5 x + 3 y\nsubject to\nwood: 2 x + 4 y <= 40\nlabor: 3 x + 2 y <= 24\nbounds\nx >= 0\ny >= 0\nend",
  "timeout_ms": 5000
}
```

### Fields

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `format` | string | Yes | - | `lp` or `mps` |
| `problem` | string | Yes | - | Problem definition |
| `timeout_ms` | int | No | 5000 | Timeout (max: 30000) |

## Solve Response Format

```json
{
  "status": "optimal",
  "objective": 40,
  "variables": {"x": 8, "y": 0},
  "solve_time_ms": 0.1,
  "iterations": 1,
  "num_vars": 2,
  "num_cons": 2
}
```

### Status Values

| Status | Description |
|--------|-------------|
| `optimal` | Optimal solution found |
| `infeasible` | No feasible solution |
| `unbounded` | Unbounded objective |
| `timeout` | Time limit reached |
| `error` | Solver error |

## Configuration

### Command Line

```bash
./ralph-solver-server [options]

Options:
  -p, --port PORT    Listen port (default: 8084)
  -h, --host HOST    Bind address (default: 0.0.0.0)
  --help             Show help
```

### Environment Variables

```bash
RALPH_PORT=8084      # Listen port
RALPH_HOST=0.0.0.0   # Bind address
```

## Limits

| Limit | Value | Description |
|-------|-------|-------------|
| Variables (LP) | 100 | Max variables for LP |
| Constraints (LP) | 100 | Max constraints for LP |
| Variables (MIP) | 50 | Max variables for MIP |
| Constraints (MIP) | 50 | Max constraints for MIP |
| Timeout | 30s | Max timeout |
| Default timeout | 5s | Default if not specified |

Limits are intentionally small - this API is for WASM demos, not production optimization.

## Build Commands

```bash
make          # Build server
make deps     # Build ralph + shared dependencies
make debug    # Debug build
make clean    # Clean artifacts
make run      # Run server
make test     # Integration test
```

## LP Format Reference

```
max: 5 x + 3 y
subject to
wood: 2 x + 4 y <= 40
labor: 3 x + 2 y <= 24
bounds
x >= 0
y >= 0
end
```

### Syntax

- `max:` or `min:` - Objective (required)
- `subject to` - Constraints section
- Constraint format: `name: expr <= | >= | = rhs`
- `bounds` - Variable bounds section (optional)
- `general` or `integer` - Integer variable declarations (for MIP)
- `binary` - Binary variable declarations (for MIP)
- `end` - End of problem

## Error Handling

HTTP status codes:
- 200: Success
- 400: Bad request (parse error, missing body, too large)
- 404: Unknown endpoint
- 408: Request timeout
- 413: Problem too large

Error response:
```json
{"error": "Parse error: missing objective section"}
```

## Architecture

```
HTTP Request
    ↓
main.c (Keel)
    ↓
ralph_api_handle() ← Transport-agnostic handler
    ↓
RalphModel → simplex → Solution
    ↓
JSON Response
```

The server is a thin wrapper around the transport-agnostic `ralph_api_handle()` function, which can also be used by WASM or direct C calls.

## Dependencies

- **ralph** - LP/MIP solver library
- **shared** - Arena allocator
- **Keel** - HTTP server (vendor submodule, MIT)
- **pthread** - Thread support

## Performance Notes

- Stateless: no data loading, instant startup
- Sub-second solve times for small problems
- Single-threaded request handling
- No caching needed (independent requests)
