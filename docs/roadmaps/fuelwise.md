# FuelWise Roadmap

## Overview

FuelWise is the truck refueling optimization engine. This roadmap covers:
1. **Shared Math Primitives** - Reusable functions in `shared/` for piecewise-linear, step functions, polynomials, distributions
2. **Weight-Dependent Consumption Model** - Realistic fuel consumption based on cargo weight
3. **Validation Benchmark** - Problem generator and constraint checker for correctness testing

---

## Chapter 0: Shared Math Primitives

### Motivation

FuelWise needs several mathematical primitives that are useful across OTTO:
- **Piecewise-linear functions**: Consumption curves, speed profiles, pricing tiers
- **Step functions**: Weight profiles, zone-based rates, discrete state changes
- **Polynomials**: Curve fitting, interpolation
- **Distributions**: Problem generation, Monte Carlo simulation

These belong in `shared/` for reuse by Velo (speed profiles), Surge (demand patterns), Tempo (time-based rates), etc.

### 0.1 Piecewise-Linear Functions (`sh_piecewise.h`)

A function defined by linear segments between breakpoints:

```c
/*
 * Piecewise-linear function: f(x) defined by (x, y) breakpoints.
 * Interpolates linearly between points.
 */
typedef struct {
    int num_points;         /* Number of breakpoints (≥2) */
    int capacity;           /* Allocated capacity */
    double *x;              /* X values (sorted ascending) */
    double *y;              /* Y values at each X */
} SHPiecewiseLinear;

/* Lifecycle */
SHPiecewiseLinear *sh_pwl_create(int initial_capacity);
void sh_pwl_free(SHPiecewiseLinear *pwl);

/* Building */
int sh_pwl_add_point(SHPiecewiseLinear *pwl, double x, double y);
int sh_pwl_add_points(SHPiecewiseLinear *pwl, const double *x, const double *y, int n);

/* Evaluation */
double sh_pwl_eval(const SHPiecewiseLinear *pwl, double x);

/* Extrapolation behavior */
typedef enum {
    SH_PWL_CLAMP,           /* Clamp to first/last Y value */
    SH_PWL_EXTRAPOLATE,     /* Extend first/last segment */
    SH_PWL_NAN              /* Return NaN outside range */
} SHPwlExtrapolation;

double sh_pwl_eval_ex(const SHPiecewiseLinear *pwl, double x, SHPwlExtrapolation mode);

/* Integration: ∫f(x)dx from a to b */
double sh_pwl_integrate(const SHPiecewiseLinear *pwl, double a, double b);

/* Utilities */
double sh_pwl_min_x(const SHPiecewiseLinear *pwl);
double sh_pwl_max_x(const SHPiecewiseLinear *pwl);
double sh_pwl_min_y(const SHPiecewiseLinear *pwl);
double sh_pwl_max_y(const SHPiecewiseLinear *pwl);

/* Serialization */
int sh_pwl_to_json(const SHPiecewiseLinear *pwl, char *buf, size_t size);
```

**Use Cases:**
- FuelWise: Consumption vs weight curves
- Velo: Speed vs gradient profiles
- Tempo: Time-of-day rate multipliers
- Quota: Volume-based pricing tiers

### 0.2 Step Functions (`sh_stepfunc.h`)

A function that changes value at discrete points:

```c
/*
 * Step function: f(x) = y_i for x in [x_i, x_{i+1})
 * Value is constant between breakpoints.
 */
typedef struct {
    int num_steps;          /* Number of steps */
    int capacity;           /* Allocated capacity */
    double *x;              /* Step boundaries (sorted ascending) */
    double *y;              /* Value in each interval */
    double initial_value;   /* Value for x < x[0] */
} SHStepFunc;

/* Lifecycle */
SHStepFunc *sh_step_create(double initial_value, int initial_capacity);
void sh_step_free(SHStepFunc *sf);

/* Building: add a step at position x that changes value by delta */
int sh_step_add(SHStepFunc *sf, double x, double delta);

/* Building: set absolute value starting at x */
int sh_step_set(SHStepFunc *sf, double x, double value);

/* Evaluation */
double sh_step_eval(const SHStepFunc *sf, double x);

/* Integration: ∫f(x)dx from a to b */
double sh_step_integrate(const SHStepFunc *sf, double a, double b);

/* Combine with piecewise-linear: ∫g(f(x))dx from a to b
 * where f is step function (e.g., weight(x)) and g is piecewise-linear (e.g., consumption(weight))
 * Computes the integral of the composition g∘f, NOT the product g*f.
 * This is the core calculation for weight-dependent fuel consumption. */
double sh_step_pwl_integrate(
    const SHStepFunc *sf,
    const SHPiecewiseLinear *pwl,
    double a,
    double b
);

/* Utilities */
double sh_step_min(const SHStepFunc *sf);  /* Minimum value over all intervals */
double sh_step_max(const SHStepFunc *sf);  /* Maximum value over all intervals */
int sh_step_count_changes(const SHStepFunc *sf, double a, double b);  /* Steps in range */
```

**Use Cases:**
- FuelWise: Weight profile (cargo changes at stops)
- Velo: Speed limits by zone
- Tempo: Shift schedules, driver availability
- Pulse: Vehicle state (loaded/empty/maintenance)

### 0.3 Polynomials (`sh_poly.h`)

Polynomial evaluation and operations:

```c
/*
 * Polynomial: f(x) = c[0] + c[1]*x + c[2]*x^2 + ... + c[n]*x^n
 */
typedef struct {
    int degree;             /* Polynomial degree */
    double *coef;           /* Coefficients c[0] to c[degree] */
} SHPoly;

/* Lifecycle */
SHPoly *sh_poly_create(int degree);
SHPoly *sh_poly_from_coeffs(const double *coef, int degree);
void sh_poly_free(SHPoly *p);

/* Evaluation using Horner's method (numerically stable) */
double sh_poly_eval(const SHPoly *p, double x);

/* Derivative: returns new polynomial */
SHPoly *sh_poly_derivative(const SHPoly *p);

/* Integral: returns new polynomial with constant term = 0 */
SHPoly *sh_poly_integral(const SHPoly *p);

/* Definite integral: ∫f(x)dx from a to b */
double sh_poly_integrate(const SHPoly *p, double a, double b);

/* Root finding (real roots only, degree ≤ 4)
 * Uses closed-form solutions: linear, quadratic formula, Cardano, Ferrari.
 * Returns -1 for degree > 4 (use numerical solver if needed).
 * Returns number of real roots found. */
int sh_poly_roots(const SHPoly *p, double *roots, int max_roots);

/* Fitting: least squares fit to (x, y) data */
SHPoly *sh_poly_fit(const double *x, const double *y, int n, int degree);
```

**Use Cases:**
- FuelWise: Smooth consumption curves (alternative to piecewise)
- Velo: Elevation profiles
- Ralph: Objective function representation

### 0.4 Probability Distributions (`sh_dist.h`)

Random sampling and density functions with **pluggable RNG backends**.

#### RNG Backend Interface

Different applications need different entropy quality:
- **Benchmarks/simulation**: Fast, reproducible (xorshift128+)
- **Cryptographic**: Secure but slower (system entropy)
- **Testing**: Deterministic sequence for reproducibility

```c
/* RNG backend types */
typedef enum {
    SH_RNG_XORSHIFT128,     /* Fast, 2^128-1 period, good for simulation */
    SH_RNG_PCG64,           /* Better statistical quality, still fast */
    SH_RNG_SPLITMIX64,      /* Simple, good for seeding other generators */
    SH_RNG_SYSTEM           /* /dev/urandom or CryptGenRandom - secure but slow */
} SHRngType;

/* Opaque RNG handle (backend-agnostic) */
typedef struct SHRng SHRng;

/* Lifecycle */
SHRng *sh_rng_create(SHRngType type);
SHRng *sh_rng_create_default(void);  /* Returns SH_RNG_XORSHIFT128 */
void sh_rng_free(SHRng *rng);

/* Seeding */
void sh_rng_seed(SHRng *rng, uint64_t seed);
void sh_rng_seed_bytes(SHRng *rng, const void *data, size_t len);
void sh_rng_seed_time(SHRng *rng);   /* Seed from current time + pid */
void sh_rng_seed_system(SHRng *rng); /* Seed from /dev/urandom */

/* Core generation */
uint64_t sh_rng_next_u64(SHRng *rng);
uint32_t sh_rng_next_u32(SHRng *rng);
double sh_rng_uniform(SHRng *rng);          /* U(0, 1) */
double sh_rng_uniform_range(SHRng *rng, double a, double b);  /* U(a, b) */
int sh_rng_int_range(SHRng *rng, int a, int b);  /* Uniform integer [a, b] */

/* State save/restore (for reproducibility) */
size_t sh_rng_state_size(const SHRng *rng);
void sh_rng_save_state(const SHRng *rng, void *buf);
void sh_rng_restore_state(SHRng *rng, const void *buf);
```

#### Distribution Sampling

```c
/* Continuous distributions */
double sh_rng_normal(SHRng *rng, double mean, double stddev);
double sh_rng_exponential(SHRng *rng, double rate);
double sh_rng_gamma(SHRng *rng, double shape, double scale);
double sh_rng_beta(SHRng *rng, double alpha, double beta);
double sh_rng_lognormal(SHRng *rng, double mu, double sigma);
double sh_rng_weibull(SHRng *rng, double shape, double scale);

/* Discrete distributions */
int sh_rng_poisson(SHRng *rng, double lambda);
int sh_rng_binomial(SHRng *rng, int n, double p);
int sh_rng_geometric(SHRng *rng, double p);

/* Array operations */
void sh_rng_shuffle(SHRng *rng, void *array, size_t n, size_t elem_size);
int sh_rng_choice(SHRng *rng, const double *weights, int n);  /* Weighted random choice */
void sh_rng_sample(SHRng *rng, int n, int k, int *out);       /* Sample k from [0,n) without replacement */
```

#### PDF/CDF Functions (stateless)

```c
/* Normal distribution */
double sh_dist_normal_pdf(double x, double mean, double stddev);
double sh_dist_normal_cdf(double x, double mean, double stddev);
double sh_dist_normal_quantile(double p, double mean, double stddev);  /* Inverse CDF */

/* Gamma distribution */
double sh_dist_gamma_pdf(double x, double shape, double scale);
double sh_dist_gamma_cdf(double x, double shape, double scale);

/* Exponential distribution */
double sh_dist_exponential_pdf(double x, double rate);
double sh_dist_exponential_cdf(double x, double rate);
```

#### Backend Comparison

| Backend | Period | Speed | Quality | Use Case |
|---------|--------|-------|---------|----------|
| `XORSHIFT128` | 2^128-1 | Very fast | Good | Benchmarks, games |
| `PCG64` | 2^128 | Fast | Excellent | Simulation, Monte Carlo |
| `SPLITMIX64` | 2^64 | Fastest | Fair | Seeding, simple use |
| `SYSTEM` | N/A | Slow | Cryptographic | Security-sensitive |

**Use Cases:**
- FuelWise benchmark: Station gap distribution, price variation
- Surge: Demand generation, service time sampling
- Simulation: Monte Carlo analysis
- Testing: Random test case generation

### 0.5 Implementation Files

| File | Purpose |
|------|---------|
| `shared/include/sh_piecewise.h` | Piecewise-linear function API |
| `shared/include/sh_stepfunc.h` | Step function API |
| `shared/include/sh_poly.h` | Polynomial API |
| `shared/include/sh_dist.h` | RNG and distribution API |
| `shared/src/sh_piecewise.c` | Piecewise-linear implementation |
| `shared/src/sh_stepfunc.c` | Step function implementation |
| `shared/src/sh_poly.c` | Polynomial implementation |
| `shared/src/sh_dist.c` | Distribution implementation |
| `shared/tests/test_shared.c` | All math primitive tests (integrated into existing test file) |

### 0.6 Test Cases

**Piecewise-Linear:**
- Interpolation between points
- Extrapolation (clamp, extend, NaN)
- Integration over segment
- Integration spanning multiple segments
- Single point, two points, many points
- Vertical segments (same X, different Y - should error)

**Step Functions:**
- Evaluation at boundaries
- Evaluation between steps
- Integration over constant region
- Integration spanning multiple steps
- Combined integration with piecewise-linear (key for consumption model)
- Empty step function (just initial value)

**Polynomials:**
- Horner's method accuracy
- Derivative correctness
- Integral correctness
- Definite integral vs numerical
- Quadratic/cubic root finding
- Least squares fitting

**Distributions:**
- Seed reproducibility across backends
- State save/restore round-trip
- Uniform range coverage (chi-squared test)
- Normal: mean/stddev of large sample matches expected
- Gamma: shape parameter behavior, mode location
- Exponential: memoryless property
- Poisson: mean equals variance
- Shuffle: all permutations possible (for small n)
- Weighted choice: frequency matches weights (statistical test)
- Backend comparison: same seed gives same sequence for same backend
- System backend: actually uses entropy source (different each run)

---

## Chapter 1: Weight-Dependent Consumption Model

### Motivation

Real truck fuel consumption depends heavily on cargo weight:
- Empty truck (tare ~15t): ~25 L/100km
- Fully loaded (GVW ~40t): ~35-40 L/100km
- Relationship is non-linear due to rolling resistance, air drag, and drivetrain efficiency

Current FuelWise uses constant or simple piecewise consumption. This chapter adds a physics-based model **using Chapter 0's shared primitives**.

### 1.0 Using Shared Primitives

The consumption model builds on `shared/` components from Chapter 0:

| Component | Shared Type | FuelWise Usage |
|-----------|-------------|----------------|
| Consumption curve | `SHPiecewiseLinear` | consumption(weight) → L/100km |
| Weight profile | `SHStepFunc` | weight(distance) → kg |
| Fuel calculation | `sh_step_pwl_integrate()` | ∫consumption(weight(d)) dd (composition) |

FuelWise provides thin wrappers with domain-specific validation.

### 1.1 Weight Profile Model

Truck weight changes at pickup/delivery points along the route:

```
Weight (kg)
    │
40t ┤     ┌─────────┐
    │     │         │
30t ┤ ────┘         │     ┌───────
    │               │     │
20t ┤               └─────┘
    │
15t ┼─────────────────────────────────── (tare weight)
    └────────────────────────────────────► Distance (m)
         P1        D1    P2         D2
```

**Implementation using `SHStepFunc` from `shared/`**
```c
/* FuelWise wrapper around SHStepFunc with domain validation */
typedef struct {
    SHStepFunc *step_func;       /* Underlying step function (weight vs distance) */
    double tare_weight_kg;       /* Empty truck weight (initial value) */
    double max_gvw_kg;           /* Maximum gross vehicle weight */
} FWWeightProfile;

/* Usage example */
FWWeightProfile *profile = fw_weight_profile_create(15000.0, 40000.0);  /* 15t tare, 40t max */

/* Add pickup at 100km: +8000 kg cargo */
fw_weight_profile_add_cargo(profile, 100000.0, +8000.0);

/* Add delivery at 300km: -8000 kg cargo */
fw_weight_profile_add_cargo(profile, 300000.0, -8000.0);

/* Internally uses: sh_step_add(profile->step_func, distance, delta) */
```

### 1.2 Consumption Curve Model

The consumption curve is a **static property of the truck** that maps weight → fuel consumption rate:

```
Consumption (L/100km)
    │
 36 ┤                              ● (40t max GVW)
 32 ┤                    ●
 28 ┤          ●
 24 ┼──●───────────────────────────────────────► Weight (kg)
      15t     25t      32t       40t
     (empty)
```

Heavier truck = higher consumption. The relationship is approximately linear but captured as piecewise-linear to allow for real-world data.

**Implementation using `SHPiecewiseLinear` from `shared/`**
```c
/* FuelWise wrapper around SHPiecewiseLinear with truck metadata */
typedef struct {
    SHPiecewiseLinear *pwl;     /* Underlying piecewise-linear (weight → consumption) */
    double min_weight_kg;       /* Valid range minimum */
    double max_weight_kg;       /* Valid range maximum */
    const char *truck_type;     /* Description (e.g., "EU Standard 40t") */
} FWConsumptionCurve;

/* Usage: pre-populated curve for common truck type */
FWConsumptionCurve *curve = fw_curve_eu_standard();  /* See section 1.6 */
double consumption = fw_consumption_at_weight(curve, 30000.0);  /* ~31 L/100km */

/* Usage: custom curve (e.g., specific vehicle or conditions) */
FWConsumptionCurve *custom = fw_consumption_curve_create(4);  /* capacity for 4 points */
fw_consumption_curve_add_point(custom, 12000.0, 20.0);  /* Light truck empty */
fw_consumption_curve_add_point(custom, 18000.0, 26.0);  /* Light truck loaded */
/* Query: internally uses sh_pwl_eval() to interpolate */
```

**Example Curve (European Truck)**
| Weight (kg) | Consumption (L/100km) |
|-------------|----------------------|
| 15,000 (empty) | 24.0 |
| 25,000 | 28.5 |
| 32,000 | 32.0 |
| 40,000 (max) | 36.5 |

### 1.3 Segment Consumption Calculation

The key insight: fuel consumption over a segment is the integral:

```
fuel (L) = ∫[from, to] consumption(weight(d)) / 100000 dd
```

This is exactly what `sh_step_pwl_integrate()` from `shared/` computes:

```c
/*
 * Calculate fuel consumed between two distances along route.
 *
 * Delegates to shared library's combined step+piecewise integration.
 */
double fw_calc_fuel_for_segment(
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    double from_m,
    double to_m
)
{
    /* sh_step_pwl_integrate computes: ∫ pwl(step(x)) dx
     * where step = weight profile, pwl = consumption curve
     * Result is in (L/100km * m), divide by 100000 to get liters */
    double integral = sh_step_pwl_integrate(
        profile->step_func,
        curve->pwl,
        from_m,
        to_m
    );
    return integral / 100000.0;
}
```

**Algorithm inside `sh_step_pwl_integrate()` (in `shared/`):**
```
1. Find all step boundaries in [from, to]
2. For each constant-weight sub-segment [a, b]:
   a. weight = sh_step_eval(step_func, a)
   b. consumption = sh_pwl_eval(pwl, weight)
   c. result += consumption * (b - a)
3. Return result
```

**Numerical Precision Notes:**
- Distances are in meters (large numbers), consumption in L/100km (small numbers)
- The product `consumption * distance` can lose precision for very long segments
- Mitigation: Process segments in order, accumulate in double precision
- Validation tolerance: `epsilon = 1e-6 * max(1.0, |expected|)` for relative comparison
- Real-world accuracy: ±0.1 L over 1000 km is acceptable (< 0.1% error)

### 1.4 Integration with Solver

Update `FWRefuelProblem` to include weight-dependent consumption:

```c
typedef struct {
    /* Existing fields... */
    double total_distance;
    double tank_capacity;
    double current_fuel;
    double minimum_fuel;
    double minimum_fuel_at_end;
    int num_stations;
    FWSnappedStation *stations;

    /* Weight-dependent consumption (new) */
    FWConsumptionCurve *consumption_curve;  /* NULL = use base_consumption */
    FWWeightProfile *weight_profile;        /* NULL = constant weight */

    /* Fallback for simple case */
    double base_consumption;                /* L/100km if no curve */

    /* Ending inventory valuation (new)
     * Accounts for fuel you'll need to buy later to refill the tank.
     * Set to expected future fuel price for multi-trip optimization.
     * Set to 0 to disable (minimize purchase cost only). */
    double destination_fuel_value;          /* $/L, 0 = disabled */
} FWRefuelProblem;
```

**Solver Changes**
1. Pre-compute fuel consumption between each pair of consecutive stations
2. Use these values in LP constraint generation
3. Existing `fw_calc_fuel_consumed()` updated to use new model
4. Modified objective for ending inventory valuation:

**Objective function:**
```
minimize: Σ(x[i] * price[i]) + (tank_capacity - y[final]) * destination_fuel_value
         └─── fuel purchased ───┘   └─── fuel to buy later to refill ───┘
```

Where `y[final]` is fuel remaining at destination. When `destination_fuel_value > 0`:
- Total cost includes future refill cost
- If last station is cheap relative to future price → fill up (less to buy later)
- If last station is expensive relative to future price → buy minimum (buy more later cheaper)

When `destination_fuel_value = 0`, reduces to current behavior (minimize purchase cost only).

Note: The `tank_capacity * destination_fuel_value` term is constant and doesn't affect
the optimal solution, but makes `total_cost` interpretable as "total trip fuel cost."

### 1.5 API Functions

```c
/* Create empty consumption curve (caller adds points)
 * The curve maps weight (kg) → consumption rate (L/100km)
 * Use convenience functions below for common truck types */
FWConsumptionCurve *fw_consumption_curve_create(int initial_capacity);
void fw_consumption_curve_free(FWConsumptionCurve *curve);

/* Add a point to the curve (must be added in ascending weight order) */
int fw_consumption_curve_add_point(
    FWConsumptionCurve *curve,
    double weight_kg,
    double consumption_l100km
);

/* Create a weight profile */
FWWeightProfile *fw_weight_profile_create(double tare_weight_kg, double max_gvw_kg);
void fw_weight_profile_free(FWWeightProfile *profile);

/* Add a weight event (pickup/delivery)
 * Positive delta = pickup, negative = delivery
 * Caller tracks stop associations separately if needed */
int fw_weight_profile_add_event(
    FWWeightProfile *profile,
    double distance_m,
    double weight_delta_kg
);

/* Validate weight profile (no overweight, no negative cargo) */
int fw_weight_profile_validate(const FWWeightProfile *profile, char *error_msg, size_t msg_size);

/* Memory Ownership:
 * - All *_create() functions return owned pointers; caller must call *_free()
 * - FWRefuelProblem does NOT own consumption_curve/weight_profile (caller manages)
 * - FWBenchInstance DOES own its curve/profile (fw_bench_free_instance frees them)
 */

/* Get weight at a specific distance */
double fw_weight_at_distance(const FWWeightProfile *profile, double distance_m);

/* Get consumption at a specific weight */
double fw_consumption_at_weight(const FWConsumptionCurve *curve, double weight_kg);

/* Calculate fuel for a segment considering weight changes */
double fw_calc_fuel_for_segment(
    const FWConsumptionCurve *curve,
    const FWWeightProfile *profile,
    double from_m,
    double to_m
);
```

### 1.6 Default Curves

Provide built-in curves for common truck types:

```c
/* European standard truck (40t GVW) */
FWConsumptionCurve *fw_curve_eu_standard(void);

/* US Class 8 truck (36t GVW) */
FWConsumptionCurve *fw_curve_us_class8(void);

/* Light truck (12t GVW) */
FWConsumptionCurve *fw_curve_light_truck(void);
```

### 1.7 Implementation Files

| File | Purpose |
|------|---------|
| `fuelwise/include/fw_consumption.h` | FuelWise wrappers with domain validation |
| `fuelwise/src/fw_consumption.c` | Wrapper implementation, default curves |

**Test Organization:**
- **`shared/tests/test_shared.c`**: Add math primitive tests (piecewise, step, poly, dist)
- **`fuelwise/tests/test_fuelwise.c`**: Add domain-specific tests (weight validation, curve bounds, solver integration)

The split keeps shared/ self-contained while FuelWise tests focus on trucking domain logic.

### 1.8 Test Cases

**FuelWise-specific tests (in `fuelwise/tests/test_fuelwise.c`):**
1. **Constant weight**: Backward compatibility with current behavior
2. **Single pickup**: Weight increases, consumption increases appropriately
3. **Pickup + delivery**: Weight up then down, verify fuel totals
4. **Multiple stops**: Complex weight profile with 5+ events
5. **Overweight validation**: Reject profile exceeding max GVW
6. **Negative cargo validation**: Reject profile going below tare weight
7. **Default curves**: Verify EU/US/light truck curves return sensible values
8. **Integration with solver**: Full LP solve with weight profile
9. **Ending inventory - cheap last station**: Last station $1.00/L, future $1.50/L → fills tank
10. **Ending inventory - expensive last station**: Last station $2.00/L, future $1.50/L → buys minimum
11. **Ending inventory disabled**: destination_fuel_value=0 → ignores ending fuel value

**Shared primitive tests (in `shared/tests/test_shared.c`):**
- Piecewise-linear: interpolation, extrapolation, integration
- Step function: evaluation, integration, boundary handling
- Combined step+pwl integration: accuracy, edge cases (composition `g(f(x))`)
- Polynomial: Horner evaluation, derivatives, roots ≤ degree 4
- Distributions: RNG backend reproducibility, statistical tests

---

## Chapter 2: Validation Benchmark

### Motivation

Verify FuelWise solver correctness by:
1. Generating realistic, satisfiable refueling problems
2. Solving them with FuelWise
3. Independently validating all constraints are satisfied

This is a **feasibility check**, not an optimality proof.

### 2.1 Problem Generation

**Station Distribution**

Real highway fuel stations follow a clustered pattern:
- Mean gap: ~30-50 km on highways
- Gaps are Gamma-distributed (captures clustering + occasional long gaps)
- Gamma(shape=2.5, scale=15km) → mean ~37km, mode ~22km

```c
typedef struct {
    /* Route parameters */
    double route_length_m;          /* Total route length (meters) */

    /* Station distribution (uses sh_rng_gamma from shared/) */
    double mean_station_gap_m;      /* Mean gap between stations */
    double gap_shape;               /* Gamma distribution shape (2.0-3.0 typical) */

    /* Truck parameters */
    double tank_capacity_l;         /* Tank capacity (liters) */
    double tare_weight_kg;          /* Empty truck weight */
    double max_gvw_kg;              /* Maximum gross vehicle weight */

    /* Consumption curve (uses FWConsumptionCurve wrapping SHPiecewiseLinear) */
    FWConsumptionCurve *curve;      /* NULL = use default EU truck */

    /* Weight events (uses SHStepFunc internally) */
    int num_weight_events;          /* 0 = constant weight at tare */
    double cargo_weight_mean_kg;    /* Mean cargo per pickup */
    double cargo_weight_stddev_kg;  /* Cargo weight variance */

    /* Fuel parameters */
    double min_fuel_l;              /* Minimum fuel level to maintain */
    double start_fuel_fraction;     /* Starting fuel as fraction of tank (0.3-0.8) */

    /* Price distribution (uses sh_rng_normal from shared/)
     *
     * Spatial correlation model (AR(1) process):
     *   price[i] = base_price + correlation * (price[i-1] - base_price) + noise
     *   where noise ~ Normal(0, stddev * sqrt(1 - correlation^2))
     *
     * correlation = 0: Independent prices (pure random)
     * correlation = 1: All prices identical (fully correlated)
     * Typical: 0.3-0.7 (nearby stations have similar prices)
     */
    double base_price_per_l;        /* Mean fuel price ($/L) */
    double price_stddev;            /* Price standard deviation */
    double price_correlation;       /* Spatial correlation (0-1), see above */

    /* RNG (uses SHRng from shared/ - pluggable backend) */
    SHRngType rng_type;             /* Default: SH_RNG_XORSHIFT128 */
    uint64_t seed;                  /* For reproducibility */
} FWBenchConfig;
```

**Satisfiability Guarantees**

Generated problems are guaranteed solvable:

1. **Gap check**: No gap exceeds `(tank_capacity - min_fuel) / max_consumption`
   - `max_consumption` = `sh_pwl_max_y(curve->pwl)` (highest consumption at max weight)
   - Conservative: assumes worst-case consumption for entire gap
2. **Start check**: Starting fuel can reach first station (at actual weight)
3. **End check**: Last station can reach destination (at actual weight)
4. **Insertion**: If gap too large, insert additional station(s)

**Weight-aware gap check**: For generated problems with weight profiles, the gap check
uses the maximum possible consumption from the curve. This is conservative but guarantees
solvability without needing to know the exact weight at each segment.

```c
typedef struct {
    FWRefuelProblem problem;
    FWSnappedStation *stations;
    int num_stations;
    FWConsumptionCurve *curve;
    FWWeightProfile *weight_profile;
    double total_fuel_required;     /* For reference */
} FWBenchInstance;

/* Generate a satisfiable problem instance */
int fw_bench_generate(const FWBenchConfig *config, FWBenchInstance *out);

/* Free instance resources */
void fw_bench_free_instance(FWBenchInstance *instance);

/* Preset configurations */
FWBenchConfig fw_bench_config_short_urban(void);    /* 200km, dense stations */
FWBenchConfig fw_bench_config_highway(void);        /* 800km, typical truck */
FWBenchConfig fw_bench_config_long_haul(void);      /* 2000km, sparse rural */
FWBenchConfig fw_bench_config_tight_margins(void);  /* 500km, challenging */
```

### 2.2 Constraint Validation

Independent checker (does not use solver internals):

```c
typedef struct {
    /* Overall result */
    int feasible;                   /* 1 if all constraints satisfied */

    /* Individual constraint status */
    int fuel_balance_ok;            /* All balance equations hold */
    int min_fuel_ok;                /* Never below minimum */
    int tank_capacity_ok;           /* Never exceeded tank */
    int non_negative_purchase_ok;   /* All purchases ≥ 0 */
    int reaches_destination;        /* Ends with enough fuel */
    int min_purchase_ok;            /* Meets minimum purchase (if set) */
    int stop_flags_ok;              /* Only stopped at allowed stations (MILP) */
    int stop_cost_ok;               /* Stop costs correctly applied (MILP) */

    /* Diagnostics */
    double min_fuel_observed;       /* Lowest fuel level seen */
    double max_fuel_observed;       /* Highest fuel level seen */
    int first_violation_station;    /* -1 if none, else station index */
    char error_msg[256];            /* Description of first violation */
} FWValidationResult;

/*
 * Validate a solution against problem constraints.
 *
 * This is an independent check that does not use solver internals.
 * It simulates the truck driving the route with the given purchases.
 *
 * Parameters:
 *   problem  - The refueling problem
 *   solution - The solution to validate
 *   result   - Output: detailed validation result
 *
 * Returns:
 *   1 if feasible, 0 if any constraint violated
 */
int fw_validate_solution(
    const FWRefuelProblem *problem,
    const FWRefuelSolution *solution,
    FWValidationResult *result
);
```

**Validation Algorithm**
```
fuel = problem.current_fuel
distance = 0

for i = 0 to num_stations:
    # Drive to station i
    fuel_consumed = fw_calc_fuel_for_segment(curve, profile, distance, station[i].distance)
    fuel -= fuel_consumed
    distance = station[i].distance

    # Check minimum fuel at arrival
    if fuel < min_fuel - epsilon:
        FAIL: "Arrived at station {i} with {fuel}L, below minimum {min_fuel}L"

    # Refuel
    purchase = solution.purchases[i]
    if purchase < 0:
        FAIL: "Negative purchase at station {i}"
    if purchase > 0 and min_purchase > 0 and purchase < min_purchase:
        FAIL: "Purchase {purchase}L below minimum {min_purchase}L"

    fuel += purchase

    # Check tank capacity after refuel
    if fuel > tank_capacity + epsilon:
        FAIL: "Exceeded tank capacity at station {i}"

    track min_fuel_observed, max_fuel_observed

# Drive to destination
fuel_consumed = fw_calc_fuel_for_segment(curve, profile, distance, total_distance)
fuel -= fuel_consumed

# Check arrival
if fuel < min_fuel_at_end - epsilon:
    FAIL: "Arrived at destination with {fuel}L, below minimum {min_fuel_at_end}L"

# Verify total_cost calculation
expected_cost = sum(purchase * price for each station)
if use_milp:
    expected_cost += sum(stop_cost for each station with purchase > 0)
if destination_fuel_value > 0:
    expected_cost += (tank_capacity - fuel) * destination_fuel_value  # future refill cost
if |solution.total_cost - expected_cost| > epsilon:
    FAIL: "Total cost mismatch"

PASS
```

### 2.3 Benchmark Driver

```c
typedef struct {
    int problems_generated;
    int problems_solved;
    int solutions_feasible;
    int constraint_violations;

    /* Timing (microseconds) */
    double gen_time_avg_us;
    double gen_time_min_us;
    double gen_time_max_us;

    double solve_time_avg_us;
    double solve_time_min_us;
    double solve_time_max_us;

    double validate_time_avg_us;

    /* Solution quality */
    double cost_avg;
    double cost_min;
    double cost_max;
    double stops_avg;
    double remaining_fuel_avg;
} FWBenchResults;

/*
 * Run benchmark with given configuration.
 *
 * Parameters:
 *   config      - Problem generation config
 *   num_runs    - Number of problems to generate and solve
 *   use_milp    - 1 for MILP solver, 0 for LP
 *   results     - Output: benchmark statistics
 *
 * Returns:
 *   0 on success, -1 on fatal error
 */
int fw_bench_run(
    const FWBenchConfig *config,
    int num_runs,
    int use_milp,
    FWBenchResults *results
);

/* Print results to stdout */
void fw_bench_print_results(const FWBenchResults *results, const char *scenario_name);
```

### 2.4 Implementation Files

| File | Purpose |
|------|---------|
| `fuelwise/bench/fw_bench.h` | Benchmark API |
| `fuelwise/bench/fw_bench.c` | Benchmark driver |
| `fuelwise/bench/fw_gen.c` | Problem generator (uses `sh_rng_*`, `sh_dist_*`) |
| `fuelwise/bench/fw_validate.c` | Constraint validator |
| `fuelwise/bench/main.c` | CLI entry point |
| `fuelwise/bench/Makefile` | Build rules |

Note: RNG and distributions come from `shared/` (`sh_dist.h`).

### 2.5 CLI Interface

```bash
# Run default benchmark (all scenarios, 1000 runs each)
./fuelwise-bench

# Run specific scenario
./fuelwise-bench --scenario highway --runs 5000

# Use MILP solver
./fuelwise-bench --milp

# Set seed for reproducibility
./fuelwise-bench --seed 12345

# Output JSON for analysis
./fuelwise-bench --format json > results.json

# Verbose mode (print each problem)
./fuelwise-bench --verbose --runs 10
```

### 2.6 Benchmark Scenarios

| Scenario | Route | Stations | Tank | Weight Events | Purpose |
|----------|-------|----------|------|---------------|---------|
| `short_urban` | 200 km | ~20 | 100 L | 2 | Dense stations, light cargo |
| `highway` | 800 km | ~25 | 300 L | 4 | Typical long-haul truck |
| `long_haul` | 2000 km | ~50 | 500 L | 8 | Multi-day trip, many stops |
| `tight_margins` | 500 km | ~8 | 200 L | 2 | Sparse stations, heavy cargo |
| `weight_heavy` | 600 km | ~15 | 400 L | 6 | Heavy pickups, high consumption |

### 2.7 Output Format

**Text (default)**
```
FuelWise Benchmark Results
==========================
Scenario: highway (800km, ~25 stations, 4 weight events)
Seed: 12345
Solver: LP

Generation:
  Problems generated: 1000
  Total fuel required: 245.3 L avg (180.2 - 312.8)

Solving:
  Problems solved: 1000 (100.0%)
  Solve time: 1.24 ms avg (0.42 - 5.81 ms)

Validation:
  Solutions feasible: 1000 (100.0%)
  Constraint violations: 0
  Validation time: 0.08 ms avg

Solution Quality:
  Total cost: $142.50 avg ($98.20 - $186.40)
  Stops: 2.3 avg (1 - 5)
  Remaining fuel: 45.2 L avg

Weight Profile:
  Cargo transported: 18,500 kg avg
  Max weight during trip: 34,200 kg avg
  Consumption range: 26.4 - 33.8 L/100km
```

**JSON**
```json
{
  "scenario": "highway",
  "config": {
    "route_length_m": 800000,
    "tank_capacity_l": 300,
    "num_weight_events": 4
  },
  "runs": 1000,
  "seed": 12345,
  "solver": "LP",
  "results": {
    "generated": 1000,
    "solved": 1000,
    "feasible": 1000,
    "violations": 0,
    "timing": {
      "generate_us": {"avg": 812, "min": 203, "max": 2104},
      "solve_us": {"avg": 1240, "min": 420, "max": 5810},
      "validate_us": {"avg": 82, "min": 45, "max": 156}
    },
    "quality": {
      "cost": {"avg": 142.50, "min": 98.20, "max": 186.40},
      "stops": {"avg": 2.3, "min": 1, "max": 5},
      "remaining_fuel": {"avg": 45.2, "min": 20.0, "max": 89.4}
    }
  }
}
```

### 2.8 Using Shared RNG

The benchmark uses `SHRng` from `shared/sh_dist.h`:

```c
/* In benchmark generator */
#include "sh_dist.h"

SHRng *rng = sh_rng_create(config->rng_type);
sh_rng_seed(rng, config->seed);

/* Generate station gaps using Gamma distribution */
double gap = sh_rng_gamma(rng, config->gap_shape, config->mean_station_gap_m / config->gap_shape);

/* Generate prices with spatial correlation */
double price = sh_rng_normal(rng, config->base_price_per_l, config->price_stddev);

sh_rng_free(rng);
```

---

## Chapter 3: Implementation Plan

### Phase 0: Shared Math Primitives (4-5 days)

| Day | Task |
|-----|------|
| 1 | `sh_piecewise.h/c` - Piecewise-linear functions |
| 1 | Tests for sh_piecewise |
| 2 | `sh_stepfunc.h/c` - Step functions |
| 2 | `sh_step_pwl_integrate()` - Combined integration |
| 2 | Tests for sh_stepfunc |
| 3 | `sh_poly.h/c` - Polynomials (Horner, derivatives, fitting) |
| 3 | Tests for sh_poly |
| 4 | `sh_dist.h/c` - RNG with pluggable backends |
| 4 | Distribution sampling (normal, gamma, exponential, etc.) |
| 5 | Tests for sh_dist, verify all backends |

### Phase 1: Consumption Model (2-3 days)

| Day | Task |
|-----|------|
| 1 | `fw_consumption.h` - Wrappers around shared primitives |
| 1 | `FWConsumptionCurve` wrapping `SHPiecewiseLinear` |
| 2 | `FWWeightProfile` wrapping `SHStepFunc` |
| 2 | Implement `fw_calc_fuel_for_segment()` using `sh_step_pwl_integrate()` |
| 2 | Integrate with solver (`fw_solve_refuel_lp`) |
| 3 | Update `fw_calc_fuel_consumed()` to use new model |
| 3 | Add default curves, FuelWise-specific tests |

### Phase 2: Benchmark Infrastructure (2-3 days)

| Day | Task |
|-----|------|
| 1 | Implement validator (`fw_validate.c`) |
| 1 | Implement generator (`fw_gen.c`) using `sh_rng_*`, `sh_dist_*` |
| 2 | Implement satisfiability checks |
| 2 | Implement driver and CLI (`main.c`) |
| 3 | Add preset scenarios, output formatting (text + JSON) |

### Phase 3: Integration & Testing (1-2 days)

| Day | Task |
|-----|------|
| 1 | Run benchmark, fix any issues |
| 1 | Add to CI (`make bench-fuelwise`) |
| 2 | Document, update MEMORY.md |

**Total: ~10-13 days** (including shared primitives)

---

## WASM Considerations

The shared math primitives and FuelWise consumption model are WASM-compatible:

- **No dynamic allocation in hot paths**: Curves/profiles are built once, evaluated many times
- **No thread-local storage**: RNG state is explicitly passed
- **No file I/O**: `SH_RNG_SYSTEM` backend not available in WASM (falls back to seed from JS)
- **Bounded memory**: Piecewise/step functions have explicit capacity, no unbounded growth

**WASM Demo Strategy**: The fuel demo already uses WASM. Weight-dependent consumption
adds a more realistic simulation without changing the WASM interface—only internal
calculation changes.

---

## Chapter 4: Future Work

- **Terrain model**: Consumption varies with elevation (uphill/downhill)
- **Speed model**: Consumption varies with speed profile
- **Temperature**: Cold weather increases consumption
- **Optimality testing**: Compare with known-optimal solutions on small instances
- **Stress testing**: Very large problems (1000+ stations)
- **Regression testing**: Catch solver bugs via benchmark failures

---

## Appendix: Consumption Curve Data

### European Truck (40t GVW)

Based on Volvo FH, Mercedes Actros data:

| Weight (kg) | Consumption (L/100km) | Notes |
|-------------|----------------------|-------|
| 15,000 | 24.0 | Empty (tare) |
| 20,000 | 26.5 | Light load |
| 25,000 | 28.5 | Half load |
| 30,000 | 31.0 | Heavy load |
| 35,000 | 33.5 | Near max |
| 40,000 | 36.5 | Maximum GVW |

### US Class 8 Truck (36t GVW)

Based on Freightliner Cascadia, Kenworth T680 data:

| Weight (kg) | Consumption (L/100km) | Notes |
|-------------|----------------------|-------|
| 13,600 | 28.0 | Empty (tare) |
| 18,000 | 30.5 | Light load |
| 23,000 | 33.0 | Half load |
| 28,000 | 36.0 | Heavy load |
| 33,000 | 39.0 | Near max |
| 36,000 | 42.0 | Maximum GVW |

Note: US trucks have higher consumption due to larger engines, higher speeds, and different fuel formulations.

---

## Chapter 5: Shadow Prices, Economic Interpretation & Monetization

### 5.1 LP Dual Variables (Shadow Prices)

The FuelWise LP (`fw_solve_refuel_lp`) has these constraints with economically meaningful duals:

| Constraint | Dual Variable | Economic Meaning |
|------------|---------------|------------------|
| **Fuel balance** `y[i] = current_fuel + Σx[j]` | λ_balance[i] | Marginal value of 1L starting fuel, propagated to station i |
| **Min fuel at arrival** `y[i] - consumed[i] ≥ min_fuel` | λ_min[i] | **Cost of safety reserve** at station i |
| **Tank capacity** `y[i] + x[i] ≤ capacity + consumed[i]` | λ_cap[i] | **Value of larger tank** at station i |
| **Reach destination** `Σx[i] ≥ needed` | λ_dest | **Marginal cost of trip completion** |

### 5.2 Practical Business Applications

**1. Tank Size ROI Analysis**
```
Total value of +1L tank = Σ max(0, λ_cap[i])
```
If this sum is $0.50/trip and you do 200 trips/year, a 100L larger tank saves $100/year.
Compare to tank upgrade cost for fleet-wide decisions.

**2. Safety Margin Pricing**
```
Cost of safety reserve = Σ λ_min[i]
```
Quantifies the tradeoff between safety and cost. High values at specific stations:
- Route is dangerously tight there
- Consider finding intermediate stations
- Useful for insurance/risk discussions with fleet managers
- Input to detention cost negotiations ("we had to take expensive fuel because of your delay")

**3. Station Negotiation Leverage**
The reduced costs on x[i] variables tell you how much cheaper a station must be before
you'd buy there. Use this in fuel card/network contract negotiations.

**4. Route Feasibility Warnings**
High λ_dest indicates the route is barely feasible. Alerts for dispatchers:
- Add contingency stations to the route
- Flag risky segments in driver app
- Trigger re-routing if fuel stops become unavailable

**5. Multi-Trip Optimization**
The `remaining_fuel_value` parameter already captures "opportunity cost of empty tank."
Shadow prices extend this to per-station analysis.

### 5.3 Exposing Duals via API

To expose shadow prices, add to `FWRefuelSolution`:

```c
typedef struct {
    /* Existing fields... */
    FWStatus status;
    double *purchases;
    int *stop_flags;
    double total_cost;
    double remaining_fuel;

    /* Dual values (new) */
    double *dual_min_fuel;      /* λ_min[i]: cost of safety margin at each station */
    double *dual_tank_cap;      /* λ_cap[i]: value of +1L tank at each station */
    double dual_destination;    /* λ_dest: marginal cost of reaching destination */
    int has_duals;              /* 1 if duals were computed, 0 otherwise */
} FWRefuelSolution;
```

Ralph already supports `ralph_get_dual()` for retrieving dual values.

### 5.4 Missing Constraints for FTL Trucking Monetization

#### High-Value Missing Constraints

| Constraint | Business Value | Complexity |
|------------|----------------|------------|
| **Fuel card network** | Fleets have contracts (Pilot, Love's, TA). Filter to approved stations | Low |
| **Hours of Service** | Driver must stop for rest regardless of fuel. Co-optimize timing | High (HoSE integration) |
| **Volume discounts** | ≥50 gal gets $0.05/gal off. Non-convex pricing | Medium (MILP) |
| **DEF co-purchase** | DEF consumed ~2-3% of diesel. Same stop for both | Low |
| **Reefer fuel** | Refrigerated trailers consume extra fuel for cooling | Low (add to consumption) |

#### International (EU) Specific

| Constraint | Business Value | Notes |
|------------|----------------|-------|
| **VAT recovery** | Fuel in certain countries has recoverable VAT | Filter by country, track VAT |
| **Currency optimization** | EUR/CHF/GBP/CZK pricing differences | Convert to base currency |
| **Toll corridors** | Combined fuel+toll optimization | Integrate with Velo toll data |
| **Cabotage rules** | EU rules on consecutive domestic trips | Route feasibility |

#### Quick Wins for Monetization

**1. Fuel Card Filter (Simplest)**
```c
typedef struct {
    /* ... existing fields ... */
    int *approved_station_ids;   /* NULL = all allowed */
    int num_approved;
} FWRefuelProblem;
```
Just filter `stations` array before solving. No solver changes needed.

**2. Volume Discount Tiers**
```c
typedef struct {
    double threshold_liters;     /* e.g., 189.27L (50 gal) */
    double discount_per_liter;   /* e.g., $0.013/L ($0.05/gal) */
} FWVolumeDiscount;
```
Makes problem non-convex. Handle with binary variable for "bought ≥ threshold" in MILP.

**3. Dual Fuel (Diesel + DEF)**
```c
typedef struct {
    double diesel_price;
    double def_price;
    double def_available;        /* Some stations don't have DEF */
} FWStation;
```
DEF consumption is ~2-3% of diesel. Add as parallel constraint set.

### 5.5 Benders Decomposition Fix

#### Current State

The current `fw_solve_refuel_benders()` does **exhaustive enumeration** for k ≤ 20 stations:
```c
int num_combinations = 1 << k;  /* 2^k */
for (int combo = 1; combo < num_combinations; combo++) {
    /* Solve subproblem for each z combination */
}
```

This is correct but not true Benders decomposition—it's brute force.

#### Proper Benders Implementation

True Benders iterates between:
1. **Master problem** (MIP): Choose which stations to stop at (z variables)
2. **Subproblem** (LP): Given z, optimize fuel purchases (x variables)
3. **Cuts**: Add constraints to master based on subproblem results

**Optimality Cut** (when subproblem is feasible):
```
θ ≥ c'x* + π'(b - Az)
```
where π are dual values from subproblem, θ is objective approximation in master.

**Feasibility Cut** (when subproblem is infeasible):
```
0 ≥ μ'(b - Az)
```
where μ is the Farkas ray from Ralph (`ralph_get_farkas()`).

#### Implementation Plan

```c
/* Master problem variables */
/* z[i] ∈ {0,1} - stop at station i */
/* θ - objective value approximation */

/* Iteration */
while (!converged) {
    /* 1. Solve master MIP */
    ralph_optimize(master);
    z_fixed = ralph_get_solution(master);  /* Get z values */
    θ_master = z_fixed[θ_index];

    /* 2. Solve subproblem LP with fixed z */
    build_subproblem(problem, z_fixed, &subproblem);
    ralph_optimize(subproblem);

    if (ralph_get_status(subproblem) == RALPH_STATUS_OPTIMAL) {
        /* 3a. Add optimality cut */
        double sub_obj = ralph_get_objval(subproblem);
        double *duals = ralph_get_dual(subproblem);

        /* Cut: θ ≥ sub_obj + Σ duals[i] * (rhs[i] - coef[i] * z[i]) */
        add_optimality_cut(master, sub_obj, duals, z_coefficients);

        /* Check convergence */
        if (sub_obj <= θ_master + epsilon) {
            converged = 1;
            /* z_fixed is optimal */
        }
    } else {
        /* 3b. Add feasibility cut */
        double *farkas = ralph_get_farkas(subproblem);

        /* Cut: 0 ≥ Σ farkas[i] * (rhs[i] - coef[i] * z[i]) */
        add_feasibility_cut(master, farkas, z_coefficients);
    }
}
```

#### Benefits over Enumeration

| Metric | Enumeration | Benders |
|--------|-------------|---------|
| Subproblems for k=20 | 1,048,576 | Typically 10-50 |
| Subproblems for k=30 | 1 billion | Typically 20-100 |
| Memory | O(2^k) worst case | O(k) |
| Scalability | k ≤ 20 | k ≤ 1000+ |

#### Implementation Files

| File | Changes |
|------|---------|
| `fw_refuel.c` | Replace enumeration with Benders loop |
| `ralph.h` | Already has `ralph_get_farkas()` |
| `fw_types.h` | Add Benders iteration stats to solution |

### 5.6 Implementation Priority

1. **Expose duals via API** (1 day) - Immediate value for business intelligence
2. **Fuel card filter** (0.5 day) - Simplest monetization constraint
3. **Benders fix** (2-3 days) - Enables scaling to 100+ stations
4. **Volume discounts** (1 day) - Common contract structure
5. **DEF co-purchase** (1 day) - Required for US compliance
6. **HoS integration** (3-5 days) - Requires HoSE module

---

## Chapter 6: API Enhancements

### 6.1 Dual Value Retrieval

```c
/* Get shadow prices from last solve */
int fw_get_duals(
    const FWRefuelSolution *solution,
    double *dual_min_fuel,      /* [num_stations] or NULL */
    double *dual_tank_cap,      /* [num_stations] or NULL */
    double *dual_destination    /* scalar or NULL */
);
```

### 6.2 Station Filtering

```c
/* Filter stations to approved network before solving */
int fw_filter_approved_stations(
    FWRefuelProblem *problem,
    const int *approved_ids,
    int num_approved
);
```

### 6.3 Benders Statistics

```c
typedef struct {
    int iterations;
    int optimality_cuts;
    int feasibility_cuts;
    double master_time_ms;
    double subproblem_time_ms;
} FWBendersStats;

int fw_get_benders_stats(
    const FWRefuelSolution *solution,
    FWBendersStats *stats
);

---

## Chapter 7: MIP Optimization Strategies

### 7.1 Problem Structure Analysis

**FuelWise MILP formulation:**
```
Variables:
  x[i] ∈ [0, tank_capacity]    Fuel purchased at station i (continuous)
  y[i] ∈ [0, y_upper]          Fuel level at station i (continuous)
  z[i] ∈ {0, 1}                Stop at station i (binary)

Objective:
  min Σ price[i]·x[i] + stop_cost·Σz[i]

Constraints:
  y[i] = y[i-1] + x[i-1] - consumption[i-1]   (flow balance)
  y[i] ≥ min_fuel                              (safety reserve)
  y[i] + x[i] ≤ tank_capacity + consumption[i] (tank capacity)
  x[i] ≤ tank_capacity · z[i]                  (linking: buy only if stop)
  x[i] ≥ min_purchase · z[i]                   (minimum purchase if stop)
```

**Key structural properties:**
1. **Path structure**: Variables form a sequence along the route
2. **Clean linking**: z[i] only affects bounds on x[i] (RHS-only coupling)
3. **Network flow subproblem**: Given z, the LP is a simple path flow
4. **Small binary count**: k binary variables for k stations

### 7.2 Optimization Approaches

#### Approach 1: Domain-Specific Cuts + B&B Priorities

**Reach cuts:** "Must stop at least once in [i, j] to have enough fuel to reach j+1"

```c
/*
 * Generate reach cuts based on problem structure.
 * These are valid inequalities that can be added upfront or lazily.
 */
void fw_generate_reach_cuts(
    const FWRefuelProblem *problem,
    int *cut_starts,      /* Cut i covers stations [cut_starts[i], cut_ends[i]] */
    int *cut_ends,
    int *num_cuts
) {
    double fuel = problem->tank_capacity;  /* Assume full tank at start */
    int segment_start = 0;

    for (int i = 0; i < problem->num_stations; i++) {
        double consumed = fw_calc_fuel_consumed(problem,
            (i == 0) ? 0 : problem->stations[i-1].distance_from_start,
            problem->stations[i].distance_from_start);

        fuel -= consumed;

        /* Check if we can reach station i+1 without refueling */
        double next_consumed = fw_calc_fuel_consumed(problem,
            problem->stations[i].distance_from_start,
            (i+1 < problem->num_stations)
                ? problem->stations[i+1].distance_from_start
                : problem->total_distance);

        if (fuel - next_consumed < problem->minimum_fuel) {
            /* Must stop somewhere in [segment_start, i] */
            cut_starts[*num_cuts] = segment_start;
            cut_ends[*num_cuts] = i;
            (*num_cuts)++;

            /* Assume we refuel to full */
            fuel = problem->tank_capacity;
            segment_start = i + 1;
        }
    }
}

/*
 * Add reach cut to model:
 *   z[start] + z[start+1] + ... + z[end] >= 1
 */
void fw_add_reach_cut(RalphModel *model, int z_start_idx, int start, int end) {
    int size = end - start + 1;
    int *indices = malloc(size * sizeof(int));
    double *coeffs = malloc(size * sizeof(double));

    for (int i = 0; i < size; i++) {
        indices[i] = z_start_idx + start + i;
        coeffs[i] = 1.0;
    }

    ralph_add_constraint(model, size, indices, coeffs, RALPH_GREATER_EQUAL, 1.0);

    free(indices);
    free(coeffs);
}
```

**B&B priorities:** Branch on earlier stations first, prefer cheap stations.

```c
void fw_set_branch_priorities(RalphModel *model, const FWRefuelProblem *problem, int z_start_idx) {
    int k = problem->num_stations;
    int *priorities = calloc(3 * k, sizeof(int));  /* x, y, z variables */
    int *directions = calloc(3 * k, sizeof(int));

    /* Find price percentiles for priority assignment */
    double *prices = malloc(k * sizeof(double));
    for (int i = 0; i < k; i++) {
        prices[i] = problem->stations[i].price;
    }
    double median_price = percentile(prices, k, 0.5);

    for (int i = 0; i < k; i++) {
        int z_idx = z_start_idx + i;

        /* Earlier stations get higher priority (branch first) */
        int base_priority = (k - i) * 10;

        /* Cheap stations get bonus priority (explore "stop here" first) */
        int price_bonus = (problem->stations[i].price < median_price) ? 5 : 0;

        priorities[z_idx] = base_priority + price_bonus;

        /* Branch UP first for cheap stations (try stopping there)
         * Branch DOWN first for expensive stations (try skipping) */
        directions[z_idx] = (problem->stations[i].price < median_price) ? 1 : 0;
    }

    ralph_set_branch_priorities(model, priorities);
    ralph_set_branch_directions(model, directions);

    free(priorities);
    free(directions);
    free(prices);
}
```

#### Approach 2: Benders Decomposition

**Why FuelWise is ideal for Benders:**
1. z affects only RHS of linking constraints (x[i] ≤ tank_capacity · z[i])
2. Subproblem is trivially fast (simple LP with k variables)
3. Infeasibility has clear meaning: "can't reach station j+1"
4. Cuts accumulate, tightening master over iterations

**Benders structure:**
```
Master (MIP):
  min  stop_cost·Σz[i] + θ
  s.t. z[i] ∈ {0, 1}
       feasibility cuts (from Farkas rays)
       optimality cuts (from LP duals)
       θ ≥ lower_bound

Subproblem (LP, given z_fixed):
  min  Σ price[i]·x[i]
  s.t. flow balance constraints
       y[i] ≥ min_fuel
       y[i] + x[i] ≤ tank_capacity + consumption[i]
       x[i] ≤ tank_capacity · z_fixed[i]    (RHS depends on z)
       x[i] ≥ min_purchase · z_fixed[i]     (RHS depends on z)
```

**Feasibility cut (when subproblem infeasible):**

The Farkas ray μ proves infeasibility: μ'b < 0 while μ'A ≥ 0.

For FuelWise, infeasibility means: "With these stops disabled (z[i]=0), can't reach some station."

The cut says: "At least one of these z[i] must be 1":
```
Σ_{i ∈ blocking_set} z[i] ≥ 1
```

Where blocking_set = stations whose z[i]=0 caused infeasibility.

```c
/*
 * Extract feasibility cut from Farkas ray.
 * Returns indices of z variables that must have at least one z[i]=1.
 */
int fw_extract_feasibility_cut(
    RalphModel *subproblem,
    const int *z_fixed,
    int k,
    int *blocking_set,
    int *blocking_size
) {
    double *farkas = malloc(ralph_get_num_constraints(subproblem) * sizeof(double));
    ralph_get_farkas_ray(subproblem, farkas);

    *blocking_size = 0;

    /* Find which z[i]=0 constraints contributed to infeasibility */
    for (int i = 0; i < k; i++) {
        if (z_fixed[i] == 0) {
            /* Check if the upper bound constraint x[i] ≤ 0 has positive Farkas multiplier */
            int con_idx = get_upper_bound_constraint_index(i);
            if (farkas[con_idx] > 1e-6) {
                blocking_set[(*blocking_size)++] = i;
            }
        }
    }

    free(farkas);
    return (*blocking_size > 0) ? 0 : -1;
}
```

**Optimality cut (when subproblem optimal):**

```
θ ≥ LP_obj + Σ λ_upper[i] · tank_capacity · (z[i] - z_fixed[i])
           + Σ λ_lower[i] · min_purchase · (z[i] - z_fixed[i])
```

Where λ_upper, λ_lower are duals on the linking constraints.

#### Approach 3: Hybrid (Cuts + Benders)

Best of both worlds:
1. Add reach cuts to master upfront (warm start Benders)
2. Use B&B priorities within master MIP solve
3. Use Benders iteration for convergence

### 7.3 Ralph Requirements

| Function | Purpose | Status |
|----------|---------|--------|
| `ralph_set_branch_priorities()` | Static priority per variable | **Needed** |
| `ralph_set_branch_directions()` | Preferred branch direction | **Needed** |
| `ralph_get_var_bounds()` | Query current lb/ub | **Needed** |
| `ralph_get_farkas_ray()` | Extract infeasibility certificate | Exists (verify) |
| `ralph_set_constraint_rhs()` | Modify RHS for Benders iterations | **Needed** |
| `ralph_get_dual()` | Get dual values for optimality cuts | Exists |
| `ralph_add_constraint()` | Add cuts to master | Exists |
| `ralph_warm_start()` | Reuse basis between iterations | **Needed** |

**New Ralph functions needed:**

```c
/* Branch-and-bound control */
void ralph_set_branch_priorities(RalphModel *model, const int *priorities);
void ralph_set_branch_directions(RalphModel *model, const int *directions);

/* Model introspection */
int ralph_get_var_bounds(RalphModel *model, int var, double *lb, double *ub);

/* Benders support */
int ralph_set_constraint_rhs(RalphModel *model, int constraint, double rhs);
int ralph_get_farkas_ray(RalphModel *model, double *ray);  /* Verify exists */

/* Warm start */
int ralph_save_basis(RalphModel *model, int **basis);
int ralph_load_basis(RalphModel *model, const int *basis);
```

### 7.4 Performance Comparison

| Approach | k ≤ 20 | k = 20-50 | k > 50 | Implementation |
|----------|--------|-----------|--------|----------------|
| **Enumeration** (current) | Fast | Infeasible | Impossible | Done |
| **Naive MIP** | Slow | Very slow | Impossible | Done |
| **Cuts + B&B priorities** | Fast | Moderate | Slow | ~250 LoC |
| **Benders** | Moderate | Fast | Fast | ~500 LoC |
| **Hybrid** | Fast | Fast | Fast | ~600 LoC |

**Estimated solve times (Ralph with enhancements):**

| k | Enumeration | MIP + Cuts | Benders |
|---|-------------|------------|---------|
| 10 | 1ms | 5ms | 10ms |
| 20 | 1s | 50ms | 30ms |
| 30 | - | 500ms | 100ms |
| 50 | - | 5s | 300ms |
| 100 | - | timeout | 1s |

### 7.5 Recommended Priority

| Priority | What | Impact | Effort | Shared with HoSE |
|----------|------|--------|--------|------------------|
| **P0** | B&B priorities | High for k=20-40 | ~100 LoC | ✅ Yes |
| **P1** | Reach cuts (preprocessing) | High | ~150 LoC | ❌ No |
| **P1** | `ralph_set_constraint_rhs()` | Medium (Benders) | ~30 LoC | ❌ No |
| **P2** | `ralph_get_farkas_ray()` | High (Benders) | ~200 LoC | ❌ No |
| **P2** | Full Benders loop | Very High for k>30 | ~400 LoC | ❌ No |

**Recommendation:**

1. **Start with P0 (priorities)** - Already needed for HoSE, immediate benefit
2. **Add reach cuts (P1)** - Big impact, no Ralph changes needed
3. **Evaluate performance** at k=30-50 with cuts + priorities
4. **If still slow, implement Benders (P2)** - Transformative for large k

### 7.6 Can Ralph Be Competitive with HiGHS?

**For FuelWise specifically: Yes, likely.**

| Factor | Assessment |
|--------|------------|
| Problem structure | Very favorable (path, few binaries) |
| Reach cuts | Extremely effective (like subtour elimination) |
| LP subproblem | Tiny and fast |
| HiGHS advantage | Generic presolve, parallel B&B |
| Ralph advantage | Domain cuts, no overhead |

**Estimate:** With reach cuts + priorities, Ralph should be within 2-5× of HiGHS for k ≤ 50.

For k > 50, Benders becomes necessary regardless of solver. At that point, Ralph's Benders implementation could actually **beat** HiGHS's generic MIP because:
1. Subproblem structure is exploited (network flow LP)
2. Cuts are domain-specific (not generic Gomory)
3. No branch-and-cut overhead on subproblems

### 7.7 Implementation Files

| File | Changes |
|------|---------|
| `fuelwise/src/fw_refuel.c` | Add reach cuts, B&B priorities, Benders loop |
| `fuelwise/include/fw_types.h` | Add cut/Benders statistics to solution |
| `ralph/src/branch_bound.c` | Add priority/direction support |
| `ralph/src/ralph.c` | Add `ralph_set_constraint_rhs()`, verify Farkas |
| `ralph/include/ralph.h` | New API declarations |

### 7.8 Test Plan

**Cuts + Priorities:**
1. k=20: Compare solve time with/without cuts
2. k=30: Verify solves within 1s
3. k=40: Verify solves within 5s
4. Correctness: All solutions pass `fw_validate_solution()`

**Benders:**
1. k=20: Compare iterations vs enumeration
2. k=50: Verify solves within 1s
3. k=100: Verify solves within 5s
4. Infeasibility: Verify Farkas cuts block infeasible z patterns
5. Optimality: Verify final solution matches enumeration (for k≤20)

---

## Chapter 8: GLPK Comparison Benchmark (Feb 2026)

### 8.1 Methodology

Added `--glpk` flag to `fuelwise-bench` that exports each MILP (without domain hints) as
an LP file via `fw_export_milp_lp()`, solves with `glpsol`, and compares objectives and
timing against Ralph (with domain hints: reach cuts, branching priorities/directions).

Both solvers solve the identical constraint set; Ralph additionally uses domain-specific
MIP hints that GLPK cannot.

### 8.2 Results (10 runs per scenario, Feb 2026)

**Current (post P5/P6 re-land with infeasibility guards, presolve 0x110F):**

| Scenario | ~Stations | Ralph avg | GLPK avg | Speedup | Obj Match |
|----------|-----------|-----------|----------|---------|-----------|
| milp15 | ~15 | **0.85 ms** | 7.45 ms | **9.9x Ralph** | 2/10 |
| milp30 | ~30 | **2.73 ms** | 9.38 ms | **4.1x Ralph** | 5/10 |
| milp50 | ~50 | **9.28 ms** | 12.26 ms | **1.4x Ralph** | 2/10 |
| milp75 | ~75 | **33.26 ms** | 57.93 ms | **2.0x Ralph** | 3/10 |
| milp100 | ~100 | 62.05 ms | **37.21 ms** | 0.7x | 0/10 |
| milp200 | ~200 | 994.96 ms | **152.22 ms** | 0.4x | 1/10 |

Ralph wins milp15–milp75 (9.9x down to 2.0x). GLPK still faster at milp100+ (1.4–2.5x).
Zero false infeasibility across all 60 trials.

**Previous (initial baseline, pre-B&B improvements, 20 runs):**

| Scenario | ~Stations | Ralph avg | GLPK avg | Speedup | Obj Match |
|----------|-----------|-----------|----------|---------|-----------|
| milp15 | ~15 | **1.72 ms** | 6.39 ms | **5.5x Ralph** | 20/20 |
| milp30 | ~30 | 65.50 ms | **7.65 ms** | 0.5x | 20/20 |
| milp50 | ~50 | 124.14 ms | **9.64 ms** | 0.1x | 20/20 |
| milp75 | ~75 | 1231.65 ms | **19.11 ms** | 0.02x | 20/20 |
| milp100 | ~100 | 7223.45 ms | **31.88 ms** | 0.004x | 10/10 |
| milp200 | ~200 | 19472.76 ms | **43.75 ms** | 0.002x | 5/5 |

**Key observations:**
- Ralph improved dramatically since initial baseline (milp30: 65ms→2.7ms, milp75: 1232ms→33ms)
- Domain hints (priorities, directions, reach cuts) + B&B improvements (dual_reopt, HYBRID,
  PATH B LU reuse, presolve, P5 bound flipping, P6 dual steepest edge) closed the gap
- GLPK still faster at milp100+ due to mature cut generation and presolve strength
- Remaining gap at scale: ~1.4x at milp100, ~2.5x at milp200 (high variance)

### 8.3 Root Cause Analysis

Ralph's MIP solver lacks several features that GLPK uses to control tree growth:

| GLPK Feature | Ralph Status | Impact |
|--------------|-------------|--------|
| **Presolve** (probe, clique) | Lightweight (0x110F: fixed vars, empty rows/cols, singleton rows, bound tightening, shift bounds) | Medium — covers basics, lacks probing/clique |
| **Gomory/MIR cuts** | c-MIR cuts implemented | Medium — tightens LP relaxation |
| **Dual simplex** | Full dual with P5 bound flipping + P6 steepest edge; dual_reopt for B&B nodes | Low — now competitive |
| **Node selection** (best-first) | HYBRID (DFS→best-bound on incumbent) | Low — effective for FuelWise structure |
| **Symmetry breaking** | FuelWise domain hints (equal-price ordering) | Low — **DONE** |
| **Probing / clique detection** | None | Medium — finds implications of variable fixing |

### 8.4 FuelWise-Specific MIP Optimizations

#### Symmetry-Breaking Constraints

FuelWise's path structure contains exploitable symmetry. If two adjacent stations have
identical prices and the solver doesn't need both, it wastes time exploring both orderings.

**Lexicographic ordering for equal-price stations:**
```
If price[i] == price[i+1] (within tolerance):
    z[i] >= z[i+1]   (prefer earlier station)
```

This halves the search space for each pair of equal-price stations.

**Station clustering:** Group nearby stations with similar prices. If reach cuts already
require one stop in the cluster, add a symmetry-breaking constraint that selects the
cheapest (or lexicographically first) station in the cluster.

#### LP Relaxation Strengthening

**Flow cover cuts:** The linking constraint `x[i] <= tank_capacity * z[i]` creates a
knapsack-like structure. Flow cover inequalities tighten the LP relaxation:
```
For each interval [a, b] where sum(x[i]) must exceed some threshold:
    sum(x[i]) <= sum(tank_capacity * z[i]) - slack
```

**Lifted reach cuts:** Current reach cuts are `sum(z[j]) >= 1` for mandatory-stop intervals.
Coefficient lifting can strengthen these: if station j can only partially satisfy the
fuel need, its coefficient should be < 1.

#### Presolve Improvements

**Implied bounds:** If `min_fuel` constraint forces `y[i] >= L` and tank capacity forces
`y[i] + x[i] <= U`, then `x[i] <= U - L`. Tighter than `x[i] <= tank_capacity`.

**Redundant variable fixing:** If a station is dominated (more expensive than all neighbors
AND the truck can skip it), fix `z[i] = 0` before solving.

**Mandatory station detection:** If a reach cut interval has only one station, fix `z[i] = 1`.

#### Warm Starting from LP Relaxation

Solve the LP relaxation first (without binary constraints), then use the LP solution to:
1. Round fractional z values to get an initial feasible solution (incumbent)
2. Use LP basis as warm start for root node
3. Set branching priorities based on fractionality (most fractional first)

#### Benders vs Full MIP

For k > 50, Benders decomposition should outperform full MIP because:
1. Master problem has only k binary variables (no x, y)
2. Subproblem is a trivial path-flow LP
3. Domain-specific feasibility cuts (from reach analysis) warm-start the master

The current Benders implementation has a known suboptimality issue (§9.2). Fixing this
and combining with symmetry-breaking in the master would be the fastest path to competitive
performance at scale.

### 8.5 Ralph-Side Improvements (see also ralph.md §4.3)

| Improvement | Status | Expected Impact | Effort |
|-------------|--------|----------------|--------|
| ~~**Best-first node selection**~~ | **DONE** (HYBRID) | 2-5x for deep trees | ~200 LoC in branch_bound.c |
| ~~**MIR cuts**~~ | **DONE** (c-MIR) | 1.5-3x tighter relaxation | ~400 LoC |
| ~~**Dual simplex for node resolves**~~ | **DONE** (dual_reopt + P5/P6) | 2-3x per-node speedup | ~800 LoC |
| **Aggressive presolve** (probing) | Not started | 1.5-2x smaller problems | ~500 LoC |
| **Pseudocost branching** | Not started | 1.5-2x better variable selection | ~200 LoC |
| **Solution pool / incumbents** | Not started | Faster pruning from good bounds | ~150 LoC |

### 8.6 Implementation Priority

1. ~~**Symmetry-breaking** (FuelWise, ~50 LoC) — immediate, zero Ralph changes~~ **DONE** (Feb 2026)
2. ~~**Mandatory station fixing** (FuelWise, ~30 LoC) — presolve, zero Ralph changes~~ **DONE** (Feb 2026)
3. ~~**Dominated station elimination** (FuelWise, ~40 LoC) — presolve, zero Ralph changes~~ **DONE** (Feb 2026)
4. **LP relaxation warm start** (FuelWise, ~80 LoC) — incumbent from LP rounding
5. **Best-first node selection** (Ralph, ~200 LoC) — biggest generic B&B improvement
6. **Pseudocost branching** (Ralph, ~200 LoC) — replaces static priorities
7. **Fix Benders suboptimality** (Ralph, investigate) — unlocks scaling to k>100
8. **Model export for GLPK comparison** — when `ralph_write_mps()` / `ralph_write_lp()` are implemented (both declared but unimplemented), FuelWise can export its MILP model for side-by-side presolve quality and solve-time comparison against GLPK

### 8.7 Per-Component Hint Breakdown (Feb 2026)

Each MIP hint can now be toggled independently via `FW_HINT_NO_*` flags
(`fw_set_mip_hint_flags()`). Test on a 20-station problem (10 L/100km, tight
200L tank, min_purchase=15L, stop_cost=$5):

| Configuration | Time | Speedup vs Raw | Notes |
|---------------|------|----------------|-------|
| **All hints enabled** | 14.7ms | **6.6x** | Combined effect |
| No hints (raw MILP) | 96.4ms | baseline | |
| Only priorities+directions | **4.6ms** | **21x** | Most impactful single hint |
| Only symmetry breaking | 27.6ms | 3.5x | Equal-price pairs pruned |
| Only dominated elimination | 41.4ms | 2.3x | Expensive stations fixed to z=0 |
| Only reach cuts | 92.8ms | ~1x | Minimal solo impact on this problem |
| Only mandatory fixing | 94.4ms | ~1x | Minimal solo impact on this problem |

**Key findings:**

1. **Branching priorities/directions dominate** — telling Ralph to try cheap
   stations first gives 21x alone. This is Ralph's single strongest advantage
   over GLPK on small problems.

2. **Symmetry breaking is second** — 3.5x from eliminating equal-price
   symmetric solutions. Impact scales with number of equal-price pairs.

3. **Dominated elimination gives 2.3x** — fixing expensive surrounded
   stations to z=0 reduces effective problem size.

4. **Reach cuts and mandatory fixing have minimal solo impact** on this
   problem class. They matter more on very tight-tank scenarios where they
   prevent infeasible branches early.

5. **Combined 6.6x < sum of parts** — hints interact; priorities already
   guide the solver away from dominated/symmetric solutions.

### 8.8 Benders vs MILP vs GLPK Comparison (Feb 2026)

Side-by-side comparison of all three solvers on the same MILP scenarios:

| Scenario | MILP (B&B) | Benders | GLPK | MILP Solved | Benders Solved | GLPK Match |
|----------|------------|---------|------|-------------|----------------|------------|
| milp15 | **2.5ms** | 2.6ms | 6.4ms | 5/5 (100%) | 3/5 (60%) | 5/5 MILP, 2/3 Benders |
| milp30 | 47ms | 22ms | **8ms** | 5/5 (100%) | 4/5 (80%) | 5/5 MILP, 3/4 Benders |
| milp50 | 142ms | 241ms | **9ms** | 5/5 (100%) | 1/5 (20%) | 5/5 MILP, 0/1 Benders |

**Benders issues identified:**

1. **Reliability**: Benders fails on 40-80% of problems (reports INFEASIBLE or
   ERROR on problems that MILP and GLPK solve successfully). This is the known
   suboptimality/convergence issue from §9.2.

2. **Objective mismatch**: Even when Benders finds a solution, it often doesn't
   match the GLPK/MILP optimal (only 5/8 matches vs 15/15 for MILP).

3. **Speed**: Benders is faster than MILP B&B at milp30 (22ms vs 47ms) but
   slower at milp50 (241ms vs 142ms). Neither competes with GLPK at scale.

4. **MILP B&B is more reliable**: 100% solve rate across all scenarios with
   100% objective match against GLPK. The domain hints (priorities, directions,
   reach cuts, symmetry-breaking, presolve) make it the recommended path.

**Conclusion:** Benders decomposition is currently broken and should not be used
in production. The MILP solver with domain hints is correct and reliable but
slow beyond ~30 stations. For production use at scale, the priority path is:

1. Fix Ralph's B&B core (best-first node selection, pseudocost branching)
2. Add LP warm start to MILP solver
3. Only then revisit Benders (after fixing convergence issues in §9.2)

---

## Chapter 9: Known Issues & TODOs

### 8.1 Benders Decomposition Known Issues (Feb 2026)

**Issue 1: Gomory Cuts Cause Infeasibility**

When using Ralph's generic Benders solver (`ralph_solve_benders`) with FuelWise, enabling
Gomory/MIR cut generation in the master MIP solver causes the algorithm to return INFEASIBLE
even on feasible problems.

**Root cause:** Gomory cuts are generated based on the LP relaxation at each B&B node. In
Benders, the master LP relaxation is incomplete (missing the full subproblem structure),
so Gomory cuts can incorrectly cut off the optimal solution.

**Workaround:** Disable cut generation in the master solver:
```c
ctx->master_solver->max_cut_rounds = 0;
```

**Status:** Workaround applied. This is expected behavior—Benders requires custom cut
handling, not generic LP cuts on an incomplete master formulation.

---

**Issue 2: Algorithm Converges to Suboptimal Solution**

The Benders implementation runs without crashes but finds a suboptimal solution.

**Test case:** 3-station problem with expected optimal cost $74.00
**Actual result:** Benders returns $85.00 (suboptimal)

**Numerical stability fix applied:** Changed theta bounds from hardcoded ±1e9 to calculated
bounds based on problem structure:
```c
double max_fuel_value = k * tank_capacity * max_price;
double theta_bound = 100.0 * (max_fuel_value + 1.0);
```

This fixed the RALPH_STATUS_ERROR (-1) crashes but the algorithm still converges to a
suboptimal solution.

---

### 8.2 TODO: Investigate Cut Generation

The suboptimal convergence suggests issues in how Benders cuts are generated or applied.
Priority investigation areas:

| Area | Suspected Issue | Investigation Steps |
|------|-----------------|---------------------|
| **Optimality cuts** | Dual values incorrectly extracted or transformed | 1. Print dual values after each subproblem solve<br>2. Verify sign convention matches Benders formulation<br>3. Check constraint indexing (linking vs sub-only) |
| **RHS contribution** | `sub_only_rhs_contribution` calculation may be wrong | 1. Verify which constraints are classified as linking<br>2. Check RHS adjustment when z values change<br>3. Test with simpler 2-station problem |
| **Cut coefficients** | Theta coefficient or z coefficients may be wrong | 1. Print full cut before adding to master<br>2. Manually verify cut validity<br>3. Compare with textbook Benders formulation |
| **Feasibility cuts** | May be too weak or incorrectly normalized | 1. Test with problem that requires feasibility cuts<br>2. Verify Farkas ray normalization<br>3. Check blocking set extraction |

**Approach:** Create minimal test case (2 stations, explicit optimal solution), trace through
Benders iteration, verify each cut manually against theory.

---

### 8.3 Relationship to Ralph §7.8

These issues are also documented in `docs/roadmaps/ralph.md` §7.8 (Known Limitations).
The FuelWise-specific context is:

- FuelWise is the primary use case for Ralph's Benders solver
- The suboptimal convergence was discovered during FuelWise benchmark testing
- Fixes should be validated against FuelWise test cases before closing
