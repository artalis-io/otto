# FuelWise Platform Architecture

## Overview

FuelWise is a truck refueling optimization platform that finds the minimum-cost fueling strategy for long-haul routes. Built on **Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper), a zero-dependency LP/MIP solver.

The platform consists of five main components organized in a layered architecture.

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Presentation Layer                       │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    React UI (ui/)                        │    │
│  │  • Interactive map with Leaflet                          │    │
│  │  • CSV upload for stations/waypoints                     │    │
│  │  • Vehicle configuration                                 │    │
│  │  • Optimization results display                          │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
              ┌───────────────┴───────────────┐
              ▼                               ▼
┌─────────────────────────┐     ┌─────────────────────────┐
│   REST API (api/)       │     │   WASM Module (wasm/)   │
│   • mongoose HTTP       │     │   • Emscripten build    │
│   • JSON endpoints      │     │   • Browser-native      │
│   • CORS support        │     │   • JS wrapper API      │
└───────────┬─────────────┘     └───────────┬─────────────┘
            │                               │
            └───────────────┬───────────────┘
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Domain Layer                                │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │               FuelWise Library (fuelwise/)               │    │
│  │  • Geospatial: Haversine, projections                    │    │
│  │  • Route: Station filtering, polyline ops                │    │
│  │  • Refuel: LP/MILP problem formulation                   │    │
│  │  • Piecewise consumption support                         │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Solver Layer                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                 Ralph Solver (ralph/)                    │    │
│  │  • Revised Simplex (primal & dual)                       │    │
│  │  • LU factorization with Markowitz                       │    │
│  │  • Branch & Bound MIP                                    │    │
│  │  • Presolve & cutting planes                             │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

## Component Dependencies

```
ui/ ──────► api/ ──────► fuelwise/ ──────► ralph/
  │                          │
  └──────► wasm/ ───────────┘
```

- **ralph/**: Zero external dependencies (standalone C library)
- **fuelwise/**: Depends on ralph/ for optimization
- **api/**: Depends on fuelwise/, uses mongoose for HTTP
- **wasm/**: Depends on fuelwise/, uses Emscripten
- **ui/**: Depends on api/ or wasm/, uses React + Leaflet

## Data Flow

### Optimization Pipeline

```
1. Input
   ├── Stations: [id, lat, lon, price]
   ├── Waypoints: [lat, lon]
   └── Config: tank_capacity, current_fuel, mpg, min_fuel

2. Route Acquisition (External)
   └── OSRM/Google/Mapbox → Polyline coordinates

3. Station Filtering (fuelwise/fw_route)
   ├── Project stations onto polyline
   ├── Filter by perpendicular distance
   └── Sort by distance along route

4. Problem Formulation (fuelwise/fw_refuel)
   ├── Build LP model with fuel constraints
   ├── Add binary variables for MILP (if min_purchase/stop_cost)
   └── Pass to solver

5. Optimization (ralph/)
   ├── Presolve: tighten bounds, remove redundancy
   ├── Simplex: find LP optimal
   └── Branch & Bound: enforce integrality (MILP only)

6. Output
   ├── Total cost
   ├── Fuel stops: [station_id, gallons, cost]
   └── Remaining fuel at destination
```

## Mathematical Model

### LP Formulation (Continuous)

**Variables:**
- `x[i]` = gallons purchased at station i
- `y[i]` = cumulative fuel before arriving at station i

**Objective:**
```
minimize Σ price[i] * x[i]
```

**Constraints:**
```
y[i] = current_fuel + Σ(j<i) x[j]                    # Fuel balance
y[i] - fuel_consumed(0, dist[i]) ≥ min_fuel         # Min fuel at station
y[i] + x[i] - fuel_consumed(0, dist[i]) ≤ capacity  # Tank capacity
Σ x[i] ≥ total_fuel_needed - current_fuel + min_end # Reach destination
```

### MILP Extension (Discrete Stops)

**Additional Variables:**
- `z[i]` ∈ {0, 1} = 1 if stopping at station i

**Additional Constraints:**
```
x[i] ≤ capacity * z[i]           # Link purchase to stop
x[i] ≥ min_purchase * z[i]       # Minimum purchase if stopping
```

**Extended Objective:**
```
minimize Σ price[i] * x[i] + Σ stop_cost * z[i]
```

## Key Design Decisions

### 1. C-First Architecture
All core logic is in C for maximum portability:
- Native performance
- WASM compilation via Emscripten
- No runtime dependencies
- Embedded system support

### 2. Separation of Concerns
- **ralph/**: General-purpose LP/MIP solver (domain-agnostic)
- **fuelwise/**: Domain-specific refueling logic
- **api/wasm/**: Delivery mechanisms
- **ui/**: Presentation

### 3. Piecewise Consumption
Supports variable fuel efficiency via route segments:
```c
typedef struct {
    double start_distance;    // Segment start (miles)
    double cargo_weight_lbs;  // Weight in segment
    double consumption_mpg;   // Fuel efficiency
} FWRouteSegment;
```

### 4. Two-Step Filtering
For large station datasets:
1. Coarse filter with overview polyline (fast)
2. Fine snap with detailed polyline (accurate)

### 5. Abstract Routing
UI supports multiple routing providers via common interface:
- OSRM (default, free)
- Google Maps
- Mapbox
- PTV

## File Organization

```
lp-solver/
├── ralph/                  # LP/MIP Solver
│   ├── include/            # Public headers
│   ├── src/                # Implementation
│   └── tests/              # Solver tests
│
├── fuelwise/               # Refueling Library
│   ├── include/            # Public API
│   ├── src/                # Implementation
│   └── tests/              # Domain tests
│
├── api/                    # REST API
│   ├── src/                # Server code
│   └── mongoose/           # HTTP library
│
├── wasm/                   # WebAssembly
│   ├── src/                # WASM bindings
│   └── build/              # Compiled output
│
├── ui/                     # React Frontend
│   ├── src/
│   │   ├── components/     # React components
│   │   ├── services/       # API client
│   │   └── types/          # TypeScript types
│   └── public/             # Static assets
│
├── docs/                   # Architecture docs
└── Makefile                # Build orchestration
```

## Build System

```makefile
# Build everything
make all        # ralph + fuelwise

# Individual targets
make ralph      # LP/MIP solver only
make fuelwise   # Refueling library
make api        # REST API server
make wasm       # WebAssembly module (requires Emscripten)

# Testing
make test       # All tests
make test-ralph
make test-fuelwise
make test-api

# Run
make run-api    # Start API server on :8080
```

## Performance Characteristics

| Component | Typical Performance |
|-----------|-------------------|
| Station filtering | O(n × m) where n=stations, m=polyline points |
| LP solve | O(n³) worst case, typically much faster |
| MILP solve | Exponential worst case, practical for <100 stations |
| API latency | <100ms for typical problems |
| WASM load | ~500KB, <100ms initialization |

## Security Considerations

- API validates all inputs
- No SQL/command injection vectors
- CORS headers for browser security
- Input size limits (10MB max request)
- No authentication (add if deploying publicly)

## Future Extensions

1. **Multi-vehicle optimization**: Fleet routing
2. **Time windows**: Station operating hours
3. **Dynamic pricing**: Real-time price feeds
4. **Historical analysis**: Price trends
5. **Mobile app**: React Native frontend
