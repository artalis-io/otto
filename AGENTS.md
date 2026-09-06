# Agents Guide for FuelWise Platform

This document provides guidance for AI agents working on the FuelWise codebase.

## Project Overview

FuelWise is a truck fleet optimization platform that combines route planning, fuel cost optimization, and custom map rendering. Built on **Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper), a zero-dependency LP/MIP solver.

## Components

| Directory | Language | Purpose |
|-----------|----------|---------|
| `ralph/` | C | LP/MIP solver (standalone, zero dependencies) |
| `velo/` | C | OSM routing engine (Dijkstra, A*, landmarks) |
| `carta/` | C | Map tile generator (MVT, PNG) |
| `fuelwise/` | C | Refueling domain library |
| `shared/` | C | Common geo utilities |
| `vendor/` | C | Third-party libs (Keel, miniz) |
| `fuelwise/api/` | C | FuelWise REST API server |
| `velo/api/` | C | Route server REST API |
| `carta/api/` | C | Tile server REST API |
| `fuelwise/wasm/` | C + JS | WebAssembly build for browsers |
| `fuelwise/ui/` | TypeScript | React frontend with Leaflet maps |

## Repository Structure

```
fuelwise-platform/
├── ralph/                      # LP/MIP Solver
│   ├── include/                # Public headers (ralph_lp.h, ralph_mip.h)
│   ├── src/                    # Implementation (simplex.c, lu.c, branch_bound.c)
│   ├── tests/                  # Solver tests + debug utilities
│   ├── benchmarks/             # Performance benchmarks
│   └── docs/                   # Simplex, LU documentation
│
├── velo/                       # OSM Routing Engine
│   ├── include/                # Public headers (velo.h, vl_*.h)
│   ├── src/                    # Implementation (vl_route.c, vl_pbf.c)
│   ├── tests/                  # Routing tests
│   └── api/                    # Route Server REST API
│       ├── src/main.c          # HTTP server + handlers
│       └── src/polyline.c      # Google Polyline encoding
│
├── carta/                      # Map Tile Generator
│   ├── include/                # Public headers (carta.h, ct_*.h)
│   ├── src/                    # Implementation (ct_mvt.c, ct_render.c)
│   ├── tests/                  # Tile tests
│   └── api/                    # Tile Server REST API
│       ├── src/main.c          # HTTP server + handlers
│       └── ui/                 # Leaflet viewer
│
├── fuelwise/                   # Refueling Domain Library
│   ├── include/                # Public API (fuelwise.h, fw_types.h)
│   ├── src/                    # Implementation (fw_refuel.c, fw_route.c)
│   ├── tests/                  # Domain tests + artifacts
│   ├── examples/               # Usage examples
│   ├── docs/                   # API documentation
│   ├── api/                    # FuelWise REST API
│   │   └── src/main.c          # HTTP server + handlers
│   ├── wasm/                   # WebAssembly Build
│   │   ├── src/fuelwise_wasm.c # WASM entry points
│   │   └── src/fuelwise-wrapper.js
│   └── ui/                     # React Frontend
│       ├── src/components/     # MapView, FileUpload, RouteConfig
│       ├── src/services/api.ts # REST API client
│       └── src/types/          # TypeScript definitions
│
├── shared/                     # Shared Geo Utilities
│   ├── include/                # Public headers (shared.h, sh_geo.h)
│   └── src/                    # Implementation (sh_geo.c)
│
├── vendor/                     # Third-party Libraries
│   ├── keel/                   # HTTP server (submodule)
│   ├── miniz/                  # zlib compression
│   └── clay/                   # UI layout (future)
│
├── docs/                       # Architecture documentation
├── docker/                     # Docker configuration
├── Makefile                    # Top-level build orchestration
├── AGENTS.md                   # This file
└── CLAUDE.md                   # Claude Code specific instructions
```

## Build Commands

```bash
# Libraries
make all              # Build all with tests (default)
make lib              # Build libraries only (no tests)
make ralph            # LP/MIP solver
make fuelwise         # Refueling library
make shared           # Shared utilities
make velo             # Routing engine
make carta            # Tile generator

# API Servers
make fuelwise-api     # FuelWise REST API (fuelwise/api)
make velo-api         # Velo route server (velo/api)
make carta-api        # Carta tile server (carta/api)

# WebAssembly (requires Emscripten)
make wasm             # Build all WASM modules
make wasm-fuelwise    # FuelWise WASM only
make wasm-velo        # Velo WASM only
make wasm-carta       # Carta WASM only
make wasm-types       # Generate TypeScript declarations
make wasm-test        # Test WASM builds

# UI (requires Node.js)
make fuelwise-ui      # Build FuelWise UI
make fuelwise-ui-dev  # Run FuelWise UI dev server on :5173

# Testing
make test             # All tests (~190)
make test-ralph       # Solver tests (65)
make test-fuelwise    # Domain tests (32)
make test-shared      # Shared tests (23)
make test-velo        # Routing tests (39)
make test-carta       # Tile tests (33)
make test-fuelwise-api # API endpoint tests

# Running
make run-fuelwise-api # Start FuelWise API on :8080
make run-carta-api    # Show Carta tile server usage
make run-velo-api     # Show Velo route server usage
```

## Architecture Principles

1. **C-first core**: All optimization logic in C for portability
2. **Zero external dependencies**: ralph/ is completely standalone
3. **Layered design**: ralph -> fuelwise -> api/wasm -> ui
4. **Modular APIs**: Each major component has its own API server

## Key Files by Component

### Ralph (LP/MIP Solver)
- `ralph/include/ralph_lp.h` - LP public API entry point
- `ralph/include/ralph_mip.h` - MIP public API entry point
- `ralph/src/simplex.c` - Primal simplex algorithm
- `ralph/src/dual_simplex.c` - Dual simplex for re-optimization
- `ralph/src/lu.c` - LU factorization (CRITICAL for correctness)
- `ralph/src/branch_bound.c` - MIP branch and bound

### Velo (Routing Engine)
- `velo/include/velo.h` - Public API
- `velo/src/vl_route.c` - Dijkstra, A*, bidirectional routing
- `velo/src/vl_pbf.c` - OSM PBF parsing
- `velo/src/vl_landmarks.c` - ALT preprocessing
- `velo/api/src/main.c` - Route server

### Carta (Tile Generator)
- `carta/include/carta.h` - Public API
- `carta/src/ct_mvt.c` - MVT vector tile encoding
- `carta/src/ct_render.c` - PNG rasterization
- `carta/src/ct_tile.c` - Web Mercator projection
- `carta/api/src/main.c` - Tile server

### FuelWise (Refueling Domain)
- `fuelwise/include/fuelwise.h` - Unified public API
- `fuelwise/include/fw_types.h` - All type definitions
- `fuelwise/src/fw_refuel.c` - LP/MILP problem formulation
- `fuelwise/src/fw_route.c` - Station filtering and snapping
- `fuelwise/api/src/main.c` - FuelWise API server

### UI (Frontend)
- `fuelwise/ui/src/App.tsx` - Main application component
- `fuelwise/ui/src/components/MapView.tsx` - Leaflet map integration
- `fuelwise/ui/src/services/api.ts` - REST API client

## Common Tasks

### Adding a new API endpoint
1. Identify which API server (fuelwise/api, velo/api, or carta/api)
2. Add handler function in `src/main.c`
3. Add route in `ev_handler()` switch statement
4. Update startup message endpoint list
5. Test with curl: `curl -X POST http://localhost:PORT/api/v1/endpoint`

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
1. Create component in `fuelwise/ui/src/components/`
2. Import and use in parent component
3. Add styles to `App.css`
4. Test in browser at http://localhost:5173

### Adding routing features
1. Edit `velo/src/vl_route.c` for algorithm changes
2. Update `velo/api/src/main.c` for API changes
3. Run `make test-velo`

### Adding tile features
1. Edit `carta/src/ct_*.c` for rendering changes
2. Update `carta/api/src/main.c` for API changes
3. Run `make test-carta`

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

1. **MILP minimum purchase**: Simple rounding heuristic may violate indicator constraints in edge cases
2. **Node.js version**: UI built with Vite 5 requires Node 18+
3. **Large PBF files**: Memory-mapped for efficiency, but still requires ~2x file size in RAM

## Debugging Tips

### LP Infeasibility
- Check `fw_validate_problem()` output message
- Verify stations are sorted by distance_from_start
- Ensure tank_capacity allows reaching between consecutive stations
- Check minimum_fuel constraints aren't too tight

### Routing Issues
- Verify graph is loaded: check `/api/v1/stats`
- Check coordinates are within PBF bounds
- Try different profiles (some roads are restricted)

### API Issues
- Use `curl -v` to see full request/response
- Check the server log output in terminal
- Verify CORS headers for browser requests

### UI Issues
- Check browser console for errors
- Verify API server is running (`curl localhost:8080/api/v1/health`)
- Check Network tab for failed requests

## Performance Notes

| Operation | Complexity | Typical Time |
|-----------|------------|--------------|
| LP solve | O(n^3) worst | < 100ms |
| Station filtering | O(n x m) | < 50ms |
| MILP solve | Exponential | < 10s for < 50 stations |
| Route (with landmarks) | O(E log V) | 30-50ms |
| Route (without landmarks) | O(E log V) | 150-350ms |
| PNG tile (512x512) | O(features) | ~75ms |
| WASM load | - | ~100ms |
| API request | - | < 100ms total |
