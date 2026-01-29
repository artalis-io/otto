# FuelWise Refueling Library - Usage Guide

## Overview

FuelWise is a domain-specific library for truck refueling optimization. It uses LP/MILP formulation (via Ralph solver) to compute optimal refueling stops along a route, minimizing total fuel cost.

## Quick Start

```bash
# Build and test
cd fuelwise
make          # Build libfuelwise.a (also builds ralph)
make test     # Run tests (32 tests)
```

## Building FuelWise

### Basic Build

```bash
cd fuelwise
make          # Build library
make test     # Run test suite
make debug    # Debug build
make clean    # Remove artifacts
```

## Directory Structure

```
fuelwise/
├── include/          # Public headers
│   ├── fuelwise.h    # Unified public API
│   └── fw_types.h    # Data structures
├── src/              # Implementation
│   ├── fw_refuel.c   # LP/MILP formulation
│   ├── fw_route.c    # Station filtering
│   └── fw_geo.c      # Distance calculations
├── api/              # REST API server
├── wasm/             # WebAssembly build
├── ui/               # React frontend
├── tests/            # Test suite
└── examples/         # Usage examples
```

## Using FuelWise in Your Code

### Basic Refueling Optimization

```c
#include "fuelwise.h"

int main() {
    // Define fuel stations along route
    FWStation stations[] = {
        {.id = 1, .distance = 100, .price = 3.50},  // 100 miles, $3.50/gal
        {.id = 2, .distance = 250, .price = 3.20},  // 250 miles, $3.20/gal
        {.id = 3, .distance = 400, .price = 3.45},  // 400 miles, $3.45/gal
    };

    // Define the problem
    FWRefuelProblem problem = {
        .stations = stations,
        .num_stations = 3,
        .total_distance = 500,          // 500 mile route
        .tank_capacity = 100,           // 100 gallon tank
        .current_fuel = 30,             // Starting with 30 gallons
        .consumption_mpg = 6.5,         // 6.5 miles per gallon
        .minimum_fuel = 10,             // Keep at least 10 gallons
    };

    // Solve
    FWRefuelSolution solution;
    int result = fw_solve_refuel_lp(&problem, &solution);

    if (result == 0 && solution.status == FW_STATUS_OPTIMAL) {
        printf("Total cost: $%.2f\n", solution.total_cost);
        for (int i = 0; i < solution.num_purchases; i++) {
            if (solution.purchases[i] > 0.01) {
                printf("Buy %.1f gal at station %d\n",
                       solution.purchases[i], stations[i].id);
            }
        }
    }

    fw_free_solution(&solution);
    return 0;
}
```

### With Piecewise Fuel Consumption

```c
#include "fuelwise.h"

int main() {
    FWStation stations[] = {
        {.id = 1, .distance = 100, .price = 3.50},
        {.id = 2, .distance = 300, .price = 3.20},
    };

    // Define piecewise consumption (varies by cargo weight)
    FWSegment segments[] = {
        {.start = 0,   .mpg = 5.5, .weight = 45000},   // Heavy load: 5.5 mpg
        {.start = 200, .mpg = 7.0, .weight = 30000},   // Light load: 7.0 mpg
    };

    FWRefuelProblem problem = {
        .stations = stations,
        .num_stations = 2,
        .total_distance = 500,
        .tank_capacity = 100,
        .current_fuel = 30,
        .minimum_fuel = 10,
        // Piecewise consumption
        .segments = segments,
        .num_segments = 2,
    };

    FWRefuelSolution solution;
    fw_solve_refuel_lp(&problem, &solution);

    // ... use solution ...

    fw_free_solution(&solution);
    return 0;
}
```

### With Stop Costs (MILP)

```c
#include "fuelwise.h"

int main() {
    FWStation stations[] = {
        {.id = 1, .distance = 100, .price = 3.50},
        {.id = 2, .distance = 250, .price = 3.20},
    };

    FWRefuelProblem problem = {
        .stations = stations,
        .num_stations = 2,
        .total_distance = 400,
        .tank_capacity = 100,
        .current_fuel = 30,
        .consumption_mpg = 6.5,
        .minimum_fuel = 10,
        // MILP options
        .min_purchase = 10,   // Minimum 10 gallons if stopping
        .stop_cost = 5.0,     // $5 fixed cost per stop
    };

    FWRefuelSolution solution;
    fw_solve_refuel_milp(&problem, &solution);  // Use MILP solver

    // ... use solution ...

    fw_free_solution(&solution);
    return 0;
}
```

### Station Filtering

```c
#include "fuelwise.h"

int main() {
    // Stations with geographic coordinates
    FWGeoStation geo_stations[] = {
        {.id = 1, .lat = 34.05, .lon = -118.24, .price = 3.89},
        {.id = 2, .lat = 33.45, .lon = -112.07, .price = 3.45},
        {.id = 3, .lat = 32.22, .lon = -110.97, .price = 3.55},
    };

    // Route polyline
    FWCoord route[] = {
        {34.05, -118.24},  // Los Angeles
        {33.45, -112.07},  // Phoenix
        {32.22, -110.97},  // Tucson
    };

    // Filter stations to those within 5 miles of route
    FWSnappedStation *filtered;
    int count;
    fw_filter_stations(geo_stations, 3, route, 3, 5.0, &filtered, &count);

    printf("Found %d stations near route\n", count);
    for (int i = 0; i < count; i++) {
        printf("Station %d at mile %.1f\n",
               filtered[i].id, filtered[i].distance);
    }

    fw_free_snapped_stations(filtered);
    return 0;
}
```

### Compiling Your Program

```bash
# Compile with FuelWise (requires ralph)
gcc -O3 -I./include -I../ralph/include \
    myprogram.c -L. -lfuelwise -L../ralph -lralph -lm \
    -o myprogram
```

## API Reference

### Problem Setup
- `FWRefuelProblem` - Problem definition structure
- `FWStation` - Station with distance and price
- `FWGeoStation` - Station with lat/lon coordinates
- `FWSegment` - Piecewise consumption segment

### Solving
- `fw_solve_refuel_lp(problem, solution)` - Solve LP (continuous purchases)
- `fw_solve_refuel_milp(problem, solution)` - Solve MILP (with stop costs)

### Station Filtering
- `fw_filter_stations(...)` - Filter stations to route corridor

### Memory Management
- `fw_free_solution(solution)` - Free solution resources
- `fw_free_snapped_stations(stations)` - Free filtered stations

### Validation
- `fw_validate_problem(problem)` - Validate problem feasibility

## LP Formulation

The core LP model in `fw_refuel.c`:

```
Variables:
  x[i] = gallons purchased at station i
  y[i] = cumulative fuel after station i

Objective:
  minimize Σ price[i] * x[i]

Constraints:
  y[i] = y[i-1] + x[i] - consumption[i-1,i]    (fuel balance)
  y[i] >= minimum_fuel                           (minimum level)
  y[i] + x[i] <= tank_capacity                  (tank capacity)
  y[n] >= fuel_to_reach_destination             (reach destination)
```

## Data Flow

```
Input: Stations + Route Polyline
        ↓
fw_filter_stations() → Snapped stations sorted by distance
        ↓
fw_solve_refuel_lp() → Builds LP model using Ralph API
        ↓
ralph_optimize() → Solves LP
        ↓
Extract solution → Purchases array + total cost
```

## Testing

```bash
make test   # 32 tests should pass

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

## Common Issues

### Infeasibility
- Check `fw_validate_problem()` error message
- Verify stations are sorted by distance
- Check tank_capacity allows reaching between stations
- Print LP model using Ralph verbose mode

### Poor solution quality
- Ensure stations cover the entire route
- Check fuel consumption rates are realistic
- Verify minimum_fuel constraint is not too high

## Dependencies

- **Ralph**: LP/MIP solver (required)
- **Standard Library**: stdio, stdlib, string, math
