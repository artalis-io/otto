# Agents Guide for FuelWise Library

## Overview

FuelWise is a C library for truck refueling optimization. It provides geospatial utilities, station filtering, and LP/MILP formulation for finding minimum-cost fueling strategies.

## Directory Structure

```
fuelwise/
├── include/
│   ├── fuelwise.h    # Unified public API
│   ├── fw_types.h    # All type definitions
│   ├── fw_geo.h      # Geospatial functions
│   ├── fw_route.h    # Station filtering
│   └── fw_refuel.h   # Optimization API
├── src/
│   ├── fuelwise.c    # Unified API implementation
│   ├── fw_geo.c      # Haversine, projections
│   ├── fw_route.c    # Filtering & snapping
│   └── fw_refuel.c   # LP/MILP formulation
├── tests/
│   └── test_fuelwise.c  # Test suite (29 tests)
├── Makefile
├── AGENTS.md         # This file
└── CLAUDE.md
```

## Build Commands

```bash
make          # Build libfuelwise.a
make test     # Run tests (29/29 should pass)
make clean    # Clean build artifacts
```

## Key Concepts

### Data Types (`fw_types.h`)

```c
// Geographic coordinate
typedef struct { double lat, lon; } FWCoord;

// Fuel station
typedef struct {
    int id;
    FWCoord location;
    double price_per_gallon;
} FWStation;

// Station snapped to route
typedef struct {
    int station_id;
    double distance_from_start;
    double perpendicular_distance;
    double price_per_gallon;
} FWSnappedStation;

// Route segment for variable consumption
typedef struct {
    double start_distance;
    double cargo_weight_lbs;
    double consumption_mpg;
} FWRouteSegment;
```

### Optimization Problem

```c
typedef struct {
    double total_distance;
    double tank_capacity;
    double current_fuel;
    double minimum_fuel;
    double base_consumption_mpg;
    int num_stations;
    FWSnappedStation *stations;
    // Optional
    int num_segments;
    FWRouteSegment *segments;
    double min_purchase;
    double stop_cost;
} FWRefuelProblem;
```

## Module Responsibilities

### Geospatial (`fw_geo.c`)
- Haversine distance calculation
- Point-to-segment projection
- Polyline length calculation

### Route (`fw_route.c`)
- Station filtering by distance to polyline
- Station snapping (projection onto route)
- Sorting by distance along route
- Deduplication for loop-back routes

### Refuel (`fw_refuel.c`)
- LP formulation: `fw_solve_refuel_lp()`
- MILP formulation: `fw_solve_refuel_milp()`
- Problem validation: `fw_validate_problem()`
- Piecewise consumption: `fw_calc_fuel_consumed()`

## LP Formulation

**Variables:**
- `x[i]` = gallons purchased at station i
- `y[i]` = cumulative fuel before arriving at station i

**Objective:** minimize Σ price[i] × x[i]

**Constraints:**
1. Fuel balance: y[i] = current_fuel + Σ(j<i) x[j]
2. Min fuel at station: y[i] - consumed(0,i) ≥ min_fuel
3. Tank capacity: y[i] + x[i] - consumed(0,i) ≤ capacity
4. Reach destination: Σ x[i] ≥ total_needed - current + min_end

## MILP Extension

**Additional variables:**
- `z[i]` ∈ {0,1} = 1 if stopping at station i

**Additional constraints:**
- x[i] ≤ capacity × z[i] (link purchase to stop)
- x[i] ≥ min_purchase × z[i] (minimum purchase)

**Extended objective:** + Σ stop_cost × z[i]

## Public API

```c
// Station filtering
int fw_filter_stations(stations, n, polyline, max_dist, result, count);

// Optimization
int fw_solve_refuel_lp(problem, solution);
int fw_solve_refuel_milp(problem, solution);

// Validation
int fw_validate_problem(problem, error_msg, msg_size);

// Memory management
void fw_free_solution(solution);
void fw_free_snapped_stations(stations);
```

## Testing

```bash
# Run all tests
make test

# Expected output: 29/29 tests passed

# Test categories:
# - Geospatial (haversine, projection)
# - Filtering (distance, snapping)
# - LP optimization
# - MILP optimization
# - Piecewise consumption
```

## Common Tasks

### Adding a new constraint type
1. Modify `FWRefuelProblem` in `fw_types.h`
2. Add constraint in `fw_solve_refuel_lp()` and `fw_solve_refuel_milp()`
3. Update validation in `fw_validate_problem()`
4. Add tests

### Supporting a new deduplication strategy
1. Add enum value to `FWDedupStrategy` in `fw_types.h`
2. Implement in `fw_deduplicate_stations()` in `fw_route.c`
3. Add tests

## Known Limitations

1. **MILP rounding**: May violate indicator constraints (known limitation)
2. **Single route**: No multi-leg trip support
3. **Fixed prices**: No time-varying prices
