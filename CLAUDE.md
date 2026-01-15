# Claude Code Instructions for FuelWise Platform

## Project Overview

FuelWise is a truck refueling optimization platform written primarily in C with a TypeScript React frontend. It finds minimum-cost fueling strategies for long-haul trucking routes, supporting variable fuel consumption and discrete stop constraints.

Built on **Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper), a zero-dependency LP/MIP solver.

## Quick Start

```bash
# Build everything
make all && make api

# Run tests
make test

# Start servers
make run-api              # API on :8080
cd ui && npm run dev      # UI on :5173
```

## Component Summary

| Component | Location | Language | Purpose |
|-----------|----------|----------|---------|
| Ralph | `ralph/` | C | LP/MIP solver engine |
| FuelWise | `fuelwise/` | C | Refueling domain logic |
| API | `api/` | C | REST API server |
| WASM | `wasm/` | C+JS | Browser build |
| UI | `ui/` | TypeScript | React frontend |

## Key Files to Understand

### When working on optimization:
1. `fuelwise/include/fw_types.h` - Data structures
2. `fuelwise/src/fw_refuel.c` - LP/MILP formulation
3. `ralph/src/simplex.c` - Simplex algorithm
4. `ralph/src/lu.c` - LU factorization (critical)

### When working on API:
1. `api/src/main.c` - All HTTP handling
2. `fuelwise/include/fuelwise.h` - Library API

### When working on UI:
1. `ui/src/App.tsx` - Main component
2. `ui/src/services/api.ts` - API client
3. `ui/src/components/MapView.tsx` - Map integration

## Build Commands

```bash
make all            # Core libraries (ralph + fuelwise)
make api            # REST API server
make wasm           # WebAssembly (needs Emscripten)
make test           # All tests
make test-ralph     # Solver tests only
make test-fuelwise  # Domain tests only
make test-api       # API tests only
make run-api        # Start API server
make clean          # Clean build artifacts
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/health` | GET | Health check |
| `/api/v1/filter` | POST | Filter stations to route |
| `/api/v1/solve` | POST | Solve with pre-snapped stations |
| `/api/v1/optimize` | POST | Full pipeline (filter + solve) |

## Mathematical Model

The optimizer solves a linear program:

**Minimize**: Σ price[i] × x[i]  (total fuel cost)

**Subject to**:
- Fuel balance at each station
- Minimum fuel level constraints
- Tank capacity constraints
- Destination reachability

For MILP (with min_purchase or stop_cost):
- Binary stop indicators z[i]
- Indicator constraints: x[i] ≤ M × z[i]
- Minimum purchase: x[i] ≥ min_purchase × z[i]

## Data Flow

```
Input → Filter Stations → Build LP → Solve → Extract Solution
  │         │                │          │           │
  │    (fw_route.c)    (fw_refuel.c) (ralph/)  (fw_refuel.c)
  │         │                │          │           │
  └─────────┴────────────────┴──────────┴───────────┘
                    fuelwise library
```

## Critical Code Sections

### LU Factorization (`ralph/src/lu.c`)
Most sensitive code - bugs cause wrong solutions. Key functions:
- `lu_factorize()` - Initial factorization
- `lu_solve()` - Solve Bx = b
- `lu_update()` - Basis change update

### LP Formulation (`fuelwise/src/fw_refuel.c`)
Problem construction in `fw_solve_refuel_lp()`:
- Variables: x[i] (purchases), y[i] (cumulative fuel)
- Constraints built with `ralph_add_constraint()`

### Station Filtering (`fuelwise/src/fw_route.c`)
Geospatial filtering in `fw_filter_stations()`:
- Projects stations onto polyline
- Calculates perpendicular distance
- Sorts by distance along route

## Testing Guidelines

```bash
# Run all tests (should pass)
make test

# Expected results:
# - ralph: 43/43 tests passed
# - fuelwise: 29/29 tests passed
```

For debugging, create test files in `tests/debug_*.c`.

## Common Pitfalls

1. **Constraint normalization**: RHS must be non-negative
2. **Station ordering**: Must be sorted by distance_from_start
3. **MILP rounding**: Simple heuristic may violate indicator constraints
4. **Memory management**: Free solutions with `fw_free_solution()`

## Architecture Decisions

1. **C-first**: Core in C for WASM + native portability
2. **Zero dependencies**: ralph/ has no external deps
3. **Layered**: ralph → fuelwise → api/wasm → ui
4. **Piecewise consumption**: Via FWRouteSegment array

## Useful Debugging

```bash
# Test API manually
curl http://localhost:8080/api/v1/health

# Test optimization
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{"stations":[...], "route":[...], "tank_capacity":100, ...}'

# Check LP solution details
# Add verbose=1 to ralph_set_int_param() calls
```

## Performance Targets

- LP solve: < 100ms for typical problems
- Station filtering: < 50ms for 1000 stations
- API latency: < 100ms end-to-end
- WASM initialization: < 100ms
