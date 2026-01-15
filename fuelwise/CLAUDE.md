# Claude Code Instructions for FuelWise Library

## Overview

FuelWise is the domain-specific library for truck refueling optimization. It depends on the Ralph solver for LP/MILP solving.

## Quick Start

```bash
make          # Build libfuelwise.a (also builds ralph)
make test     # Run tests (29/29 should pass)
```

## Key Files

| File | Purpose |
|------|---------|
| `include/fuelwise.h` | Unified public API |
| `include/fw_types.h` | All data structures |
| `src/fw_refuel.c` | LP/MILP formulation |
| `src/fw_route.c` | Station filtering |
| `src/fw_geo.c` | Distance calculations |

## Data Flow

```
Input Stations + Polyline
        ↓
  fw_filter_stations() → Snapped stations sorted by distance
        ↓
  fw_solve_refuel_lp() → Builds LP model using Ralph API
        ↓
  ralph_optimize() → Solves LP
        ↓
  Extract solution → Purchases array + total cost
```

## Critical Functions

### `fw_solve_refuel_lp()` in `src/fw_refuel.c`
Builds the LP model:
```c
// Variables: x[i] = purchase, y[i] = cumulative fuel
// x variables: indices 0 to k-1
// y variables: indices k to 2k-1

// Constraints added:
// 1. Fuel balance (equality)
// 2. Minimum fuel at arrival (>=)
// 3. Tank capacity after refuel (<=)
// 4. Reach destination (>=)
```

### `fw_calc_fuel_consumed()` in `src/fw_refuel.c`
Calculates fuel used between two distances:
- Constant rate if no segments
- Piecewise if segments defined

### `fw_filter_stations()` in `src/fw_route.c`
Filters stations to those near route:
1. For each station, find closest point on polyline
2. If perpendicular distance ≤ max_distance, include
3. Sort by distance along route

## Common Tasks

### Modifying the LP formulation
1. Edit `fw_solve_refuel_lp()` in `src/fw_refuel.c`
2. Add/modify constraints using `ralph_add_constraint()`
3. Update `fw_validate_problem()` if new validation needed
4. Run `make test`

### Adding piecewise consumption features
1. Modify `fw_calc_fuel_consumed()` for new calculation logic
2. Update segment handling in solvers
3. Add tests for edge cases

### Debugging infeasibility
1. Check `fw_validate_problem()` error message
2. Verify stations are sorted by distance
3. Check tank_capacity allows reaching between stations
4. Print LP model using Ralph verbose mode

## Testing

```bash
make test   # 29/29 should pass

# Test categories:
# - Geospatial calculations
# - Station filtering
# - Basic LP optimization
# - MILP with stop costs
# - Piecewise consumption
```

## Memory Management

```c
// Always free solutions
FWRefuelSolution solution;
fw_solve_refuel_lp(&problem, &solution);
// ... use solution ...
fw_free_solution(&solution);

// Free filtered stations
FWSnappedStation *filtered;
fw_filter_stations(..., &filtered, &count);
// ... use filtered ...
fw_free_snapped_stations(filtered);
```

## Code Style

- 4-space indentation
- `fw_` prefix for all public functions
- `FW` prefix for all public types
- Comments for algorithm steps
