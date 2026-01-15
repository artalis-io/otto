# Agents Guide for FuelWise Platform

This document provides guidance for AI agents working on the FuelWise codebase.

## Project Overview

FuelWise is a truck refueling optimization platform that finds minimum-cost fueling strategies for long-haul routes. Built on **Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper), a zero-dependency LP/MIP solver.

It consists of five main components:

| Directory | Language | Purpose |
|-----------|----------|---------|
| `ralph/` | C | LP/MIP solver (standalone, zero dependencies) |
| `fuelwise/` | C | Refueling domain library |
| `api/` | C | REST API server (mongoose HTTP) |
| `wasm/` | C + JS | WebAssembly build for browsers |
| `ui/` | TypeScript | React frontend with Leaflet maps |

## Repository Structure

```
lp-solver/
├── ralph/                  # LP/MIP Solver
│   ├── include/            # Public headers (ralph.h, sparse.h, lp.h, mip.h)
│   ├── src/                # Implementation (simplex.c, lu.c, branch_bound.c, etc.)
│   └── tests/              # Solver tests (test_main.c)
│
├── fuelwise/               # Refueling Domain Library
│   ├── include/            # Public API (fuelwise.h, fw_types.h, fw_*.h)
│   ├── src/                # Implementation (fw_geo.c, fw_route.c, fw_refuel.c)
│   └── tests/              # Domain tests (test_fuelwise.c)
│
├── api/                    # REST API Server
│   ├── src/main.c          # HTTP server + all handlers
│   └── mongoose/           # mongoose HTTP library
│
├── wasm/                   # WebAssembly Build
│   ├── src/
│   │   ├── fuelwise_wasm.c # WASM entry points
│   │   └── fuelwise-wrapper.js  # JavaScript API
│   └── Makefile            # Emscripten build
│
├── ui/                     # React Frontend
│   ├── src/
│   │   ├── components/     # MapView, FileUpload, RouteConfig, etc.
│   │   ├── services/api.ts # REST API client
│   │   └── types/          # TypeScript definitions
│   └── public/             # Static assets
│
├── docs/                   # Architecture documentation
├── Makefile                # Top-level build orchestration
├── AGENTS.md               # This file
└── CLAUDE.md               # Claude Code specific instructions
```

## Build Commands

```bash
# Build core libraries
make all            # Build ralph + fuelwise
make ralph          # LP/MIP solver only
make fuelwise       # Refueling library only

# Build deployment targets
make api            # REST API server
make wasm           # WebAssembly (requires Emscripten)

# Testing
make test           # All tests
make test-ralph     # Solver tests (43 tests)
make test-fuelwise  # Domain tests (29 tests)
make test-api       # API endpoint tests

# Running
make run-api        # Start API on :8080
cd ui && npm run dev  # Start UI on :5173
```

## Architecture Principles

1. **C-first core**: All optimization logic in C for portability
2. **Zero external dependencies**: ralph/ is completely standalone
3. **Layered design**: ralph → fuelwise → api/wasm → ui
4. **Domain separation**: Solver logic vs domain logic vs presentation

## Key Files by Component

### Ralph (Solver)
- `ralph/include/ralph.h` - Public API entry point
- `ralph/src/simplex.c` - Primal simplex algorithm
- `ralph/src/dual_simplex.c` - Dual simplex for re-optimization
- `ralph/src/lu.c` - LU factorization (CRITICAL for correctness)
- `ralph/src/branch_bound.c` - MIP branch and bound

### FuelWise (Domain)
- `fuelwise/include/fuelwise.h` - Unified public API
- `fuelwise/include/fw_types.h` - All type definitions
- `fuelwise/src/fw_refuel.c` - LP/MILP problem formulation
- `fuelwise/src/fw_route.c` - Station filtering and snapping
- `fuelwise/src/fw_geo.c` - Haversine distance calculations

### API (HTTP Server)
- `api/src/main.c` - Complete server (handlers + routing)

### UI (Frontend)
- `ui/src/App.tsx` - Main application component
- `ui/src/components/MapView.tsx` - Leaflet map integration
- `ui/src/services/api.ts` - REST API client

## Common Tasks

### Adding a new API endpoint
1. Add handler function in `api/src/main.c`
2. Add route in `ev_handler()` switch statement
3. Update startup message endpoint list
4. Test with curl: `curl -X POST http://localhost:8080/api/v1/newpoint`

### Adding a FuelWise function
1. Declare in appropriate header (`fuelwise/include/fw_*.h`)
2. Implement in corresponding source file
3. Add tests in `fuelwise/tests/test_fuelwise.c`
4. Run `make test-fuelwise`

### Modifying the LP formulation
1. Edit `fw_solve_refuel_lp()` in `fuelwise/src/fw_refuel.c`
2. For MILP changes, edit `fw_solve_refuel_milp()` in same file
3. Update `fw_validate_problem()` if constraints change
4. Run full test suite

### Adding a UI component
1. Create component in `ui/src/components/`
2. Import and use in parent component
3. Add styles to `App.css`
4. Test in browser at http://localhost:5173

## Code Style

### C Code
- 4-space indentation
- `snake_case` for functions and variables
- `SCREAMING_CASE` for constants/macros
- Comments for non-obvious logic
- Validate inputs at API boundaries

### TypeScript
- 2-space indentation
- `camelCase` for variables/functions
- `PascalCase` for types/components
- Explicit types preferred
- Functional components with hooks

## Known Limitations

1. **MILP minimum purchase**: Simple rounding heuristic may violate indicator constraints in edge cases (documented in `ralph/src/branch_bound.c`)
2. **Node.js version**: UI built with Vite 5 requires Node 18+
3. **OSRM routing**: Uses public demo server (rate limited)

## Debugging Tips

### LP Infeasibility
- Check `fw_validate_problem()` output message
- Verify stations are sorted by distance_from_start
- Ensure tank_capacity allows reaching between consecutive stations
- Check minimum_fuel constraints aren't too tight

### API Issues
- Use `curl -v` to see full request/response
- Check mongoose debug output in terminal
- Verify CORS headers for browser requests

### UI Issues
- Check browser console for errors
- Verify API server is running (`curl localhost:8080/api/v1/health`)
- Check Network tab for failed requests

## Performance Notes

| Operation | Complexity | Typical Time |
|-----------|------------|--------------|
| LP solve | O(n³) worst | < 100ms |
| Station filtering | O(n × m) | < 50ms |
| MILP solve | Exponential | < 10s for < 50 stations |
| WASM load | - | ~100ms |
| API request | - | < 100ms total |
