# FuelWise

**Truck Refueling Optimization Platform**

FuelWise is a complete platform for optimizing truck refueling stops along a route. Given a set of fuel stations with prices and a route, it determines the optimal fueling strategy to minimize total cost while respecting tank constraints.

Built on **Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper), a zero-dependency LP/MIP solver written in C.

## Features

- **LP/MIP Optimization** - Exact solutions using the Revised Simplex method and Branch & Bound
- **Geospatial Processing** - Haversine distance, station filtering, route snapping
- **Variable Consumption** - Piecewise linear fuel consumption based on cargo weight
- **Stop Costs** - MILP formulation with minimum purchase and fixed stop costs
- **REST API** - Lightweight HTTP server for integration
- **WebAssembly** - Run optimization directly in the browser
- **React UI** - Interactive map interface with route visualization

## Quick Start

### Using Docker

```bash
# Build and run the full platform
docker build -t fuelwise .
docker run -p 80:80 -p 8080:8080 fuelwise

# Or run just the API
docker build --target api-only -t fuelwise-api .
docker run -p 8080:8080 fuelwise-api
```

### Building from Source

```bash
# Build C libraries
make all

# Run tests (43 Ralph + 29 FuelWise tests)
make test

# Start API server
make run-api

# Build and run UI (requires Node.js)
cd ui && npm install && npm run dev
```

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        FuelWise Platform                        │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │   React UI  │  │  WASM Build │  │       REST API          │  │
│  │  (Leaflet)  │  │  (Browser)  │  │      (mongoose)         │  │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬─────────────┘  │
│         │                │                     │                │
│         └────────────────┴──────────┬──────────┘                │
│                                     │                           │
│  ┌──────────────────────────────────┴────────────────────────┐  │
│  │                    FuelWise Library                        │  │
│  │  ┌──────────┐  ┌───────────┐  ┌─────────────────────────┐ │  │
│  │  │ fw_geo.c │  │fw_route.c │  │      fw_refuel.c        │ │  │
│  │  │ Haversine│  │ Filtering │  │   LP/MILP Formulation   │ │  │
│  │  └──────────┘  └───────────┘  └─────────────────────────┘ │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                     │                           │
│  ┌──────────────────────────────────┴────────────────────────┐  │
│  │                      Ralph Solver                          │  │
│  │  ┌──────────┐  ┌───────────┐  ┌─────────────────────────┐ │  │
│  │  │simplex.c │  │   lu.c    │  │    branch_bound.c       │ │  │
│  │  │ Revised  │  │    LU     │  │    Branch & Bound       │ │  │
│  │  │ Simplex  │  │Factorize  │  │    + Gomory Cuts        │ │  │
│  │  └──────────┘  └───────────┘  └─────────────────────────┘ │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Project Structure

```
fuelwise/
├── ralph/          # LP/MIP Solver (libralph.a)
├── fuelwise/       # Refueling Optimization Library (libfuelwise.a)
├── api/            # REST API Server
├── wasm/           # WebAssembly Build
├── ui/             # React Application
├── docs/           # Architecture Documentation
├── docker/         # Docker support files
└── .github/        # CI/CD workflows
```

## API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/health` | Health check |
| POST | `/api/v1/filter` | Filter stations to route |
| POST | `/api/v1/solve` | Solve optimization problem |
| POST | `/api/v1/optimize` | Full pipeline (filter + solve) |

### Example Request

```bash
curl -X POST http://localhost:8080/api/v1/optimize \
  -H "Content-Type: application/json" \
  -d '{
    "stations": [
      {"id": 1, "lat": 34.05, "lon": -118.24, "price": 3.89},
      {"id": 2, "lat": 33.45, "lon": -112.07, "price": 3.45}
    ],
    "route": [[34.05, -118.24], [33.45, -112.07]],
    "tank_capacity": 300,
    "current_fuel": 50,
    "consumption_mpg": 6.5,
    "minimum_fuel": 25
  }'
```

### Response

```json
{
  "status": "optimal",
  "total_cost": 1234.56,
  "num_stops": 3,
  "remaining_fuel": 45.2,
  "stops": [
    {"station_id": 1, "gallons": 125.5, "cost": 488.20},
    {"station_id": 2, "gallons": 150.0, "cost": 517.50}
  ]
}
```

## Configuration Options

### Vehicle Configuration

| Parameter | Type | Description |
|-----------|------|-------------|
| `tank_capacity` | float | Maximum fuel capacity (gallons) |
| `current_fuel` | float | Starting fuel level (gallons) |
| `consumption_mpg` | float | Base fuel consumption (miles/gallon) |
| `minimum_fuel` | float | Minimum fuel to maintain (gallons) |
| `min_purchase` | float | Minimum gallons per stop (optional) |
| `stop_cost` | float | Fixed cost per stop in $ (optional) |

### Variable Consumption (Piecewise)

```json
{
  "segments": [
    {"start": 0, "weight": 45000, "mpg": 5.5},
    {"start": 200, "weight": 35000, "mpg": 6.5},
    {"start": 400, "weight": 25000, "mpg": 7.5}
  ]
}
```

## Performance

- **Ralph LP Solver**: Handles problems with 1000+ variables
- **FuelWise**: Typical route optimization in <100ms
- **API**: Concurrent request handling via mongoose

## Testing

```bash
# All tests
make test

# Ralph only (43 tests)
make test-ralph

# FuelWise only (29 tests)
make test-fuelwise

# API endpoints
make test-api

# UI
cd ui && npm run lint && npm run build
```

## Development

### Prerequisites

- GCC or Clang (C99)
- GNU Make
- Node.js 20+ (for UI)
- Emscripten (for WASM, optional)

### Building Components

```bash
# Build everything
make all

# Individual components
make ralph      # Build Ralph solver
make fuelwise   # Build FuelWise library
make api        # Build REST API
make wasm       # Build WASM (requires Emscripten)

# Development server
cd ui && npm run dev
```

## Documentation

- [Architecture Overview](docs/ARCHITECTURE.md)
- [REST API Reference](docs/API.md)
- [Ralph Solver](ralph/CLAUDE.md)
- [FuelWise Library](fuelwise/CLAUDE.md)

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests: `make test`
5. Submit a pull request

## License

MIT License - see [LICENSE](LICENSE) for details.

## Author

Mark Farkas - 2026

---

Built with Ralph - **R**obust **A**I **L**inear **P**rogramming **H**elper
