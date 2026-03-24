# FuelWise Validation Benchmark

Comprehensive testing framework for FuelWise solver correctness, including problem generation, solution validation, and performance benchmarking.

## Quick Start

```bash
# Build and run validator tests
make validator-test

# Run the full regression harness
make regression-test

# Run the Ralph-vs-GLPK MILP benchmark gate
make benchmark-gate

# Profile the current worst MILP case with Ralph MIP telemetry
make benchmark-profile

# Run benchmark scenarios
make test           # All scenarios, 10 runs each
make quick          # Highway only, 5 runs

# Run with specific options
./fuelwise-bench --scenario us --runs 50 --milp
```

## Test Categories

### Validator Tests (`make validator-test`)

| Category | Tests | Description |
|----------|-------|-------------|
| **Constraint Violations** | 5 | Valid solutions pass, invalid fail |
| **Constraint Enforcement** | 2 | MILP min_purchase, minimum fuel level |
| **Economic Optimality** | 5 | Buy cheap, skip expensive, minimize cost |
| **Consistency Checks** | 4 | Fuel balance, infeasibility detection |
| **Weight-Dependent** | 4 | Consumption curves, weight profiles |
| **Edge Cases** | 3 | Zero/one station, piecewise segments |
| **Stress Tests** | 2 | 100+ stations, imperial units |
| **Final Leg & Remaining Fuel** | 3 | Final leg consumption, remaining fuel value |
| **Stop Costs** | 3 | Stop cost in total, MILP consolidation |
| **Advanced** | 4 | Determinism, numerical edge cases, weight-dependent solving, API round-trip |

### Full Regression Harness (`make regression-test`)

Runs:
- `fuelwise/bench/test_validator`
- `ralph` complete LP regression suite
- `ralph` complete MIP regression suite

This is the full cross-project regression gate for FuelWise MILP correctness work.

### Benchmark Gate (`make benchmark-gate`)

Runs the fixed MILP matrix:
- scenarios: `milp30`, `milp50`, `milp75`, `milp100`, `milp200`
- seeds: `42`, `123`
- solver mode: Ralph MILP vs GLPK

The gate:
- saves JSON artifacts under `/tmp/fuelwise-benchmark-gate` by default
- fails on solve, validation, or objective-parity regressions
- warns on large Ralph-vs-GLPK slowdown ratios

Raw apples-to-apples mode is also available:
```bash
make benchmark-gate-raw
```

### Focused Performance Profile (`make benchmark-profile`)

Runs a single focused MILP benchmark case and writes two artifacts:
- aggregate benchmark JSON for the configured case
- Ralph MIP phase telemetry for one deterministic run of that same case

Defaults:
- scenario: `milp75`
- seed: `123`
- aggregate runs: `5`
- telemetry runs: `1`

Artifacts are written under `/tmp/fuelwise-benchmark-profile` by default.

Useful overrides:
```bash
make benchmark-profile PROFILE_SCENARIO=milp100 PROFILE_SEED=42
make benchmark-profile-raw PROFILE_SCENARIO=milp75 PROFILE_SEED=123
```

### Benchmark Scenarios

| Scenario | Route | Tank | Description |
|----------|-------|------|-------------|
| `urban` | 1500 km | 400 L | Dense stations, 3-day regional |
| `highway` | 3000 km | 500 L | Cross-country corridor (default) |
| `long` | 5000 km | 800 L | Transcontinental, sparse rural |
| `tight` | 2500 km | 350 L | Sparse infrastructure, tight margins |
| `us` | 2000 mi | 300 gal | US Interstate, Class 8 truck |

## Architecture

```
test_validator.c   # FuelWise validator and regression tests
fw_bench.c         # Benchmark driver, statistics
fw_gen.c           # Problem generation, preset configs
fw_validate.c      # Independent solution validator
main.c             # CLI interface
```

### Validation vs Solver

The validator is **independent** of the solver:
- Simulates truck driving the route with given purchases
- Checks all constraints: fuel balance, minimum levels, tank capacity
- Uses same tolerances as solver (0.5L for stop threshold)

This catches both solver bugs AND validator bugs.

## Tolerances

| Parameter | Value | Notes |
|-----------|-------|-------|
| Fuel level | 0.1% relative | Min 0.1L absolute |
| Stop threshold | 0.5L | Matches solver |
| Cost | 0.01 | Currency precision |

LP numerical noise (tiny purchases like 0.04L) is handled gracefully.

## Unit Systems

Internal representation is always metric (meters, liters, L/100km, kg). Imperial scenarios convert at boundaries:

```c
// US Interstate config
cfg.route_length_m = sh_miles_to_m(2000);         // 2000 mi
cfg.tank_capacity_l = sh_gallons_to_liters(300);  // 300 gal
cfg.base_price_per_l = sh_price_per_gallon_to_liter(3.50);  // $3.50/gal
cfg.units = SH_UNITS_IMPERIAL;  // For display only
```

## Adding Tests

1. Add test function following existing pattern:
   ```c
   void test_my_feature(void)
   {
       printf("\n=== Test: My Feature ===\n");
       // Setup problem
       // Solve
       // ASSERT(condition, "message");
       // Cleanup
   }
   ```

2. Call from `main()` in appropriate category

3. Run: `make validator-test`

## Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| LP solve (2-5 stations) | <5 ms | Typical benchmark |
| LP solve (100+ stations) | <100 ms | Stress test |
| MILP solve (3 stations) | <5 s | With min_purchase |
| Validation | <1 ms | Per solution |

## Output Formats

### Text (default)
```
FuelWise Benchmark Results
==========================
Scenario: highway (3000km, seed=12345)

Solver Performance:
  Problems solved: 100/100 (100.0%)
  Solve time: 2.34 ms avg (1.89 - 4.12 ms)

Validation:
  Solutions feasible: 100/100 (100.0%)
  Validation time: 0.45 ms avg

Solution Quality:
  Total cost: $1234.56 avg ($1100.00 - $1400.00)
  Stops: 4.2 avg
```

### JSON (`--json`)
```json
{
  "scenario": "highway",
  "seed": 12345,
  "route_length_km": 3000.0,
  "solver": {
    "solved": 100,
    "feasible": 100,
    "solve_time_ms": {"avg": 2.34, "min": 1.89, "max": 4.12}
  },
  "solution": {
    "cost": {"avg": 1234.56, "min": 1100.00, "max": 1400.00},
    "stops_avg": 4.2
  }
}
```

## CI Integration

The benchmark is integrated into the FuelWise test suite:

```bash
make -C fuelwise test  # Runs both unit tests AND validator tests
```

To run only validator tests:
```bash
make -C fuelwise/bench validator-test
```

To run the full regression harness:
```bash
make -C fuelwise/bench regression-test
```
