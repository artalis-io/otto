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
     * Credits remaining fuel at destination at this price.
     * Set to expected future fuel price for multi-trip optimization.
     * Set to 0 to disable (minimize cost only, ignore ending fuel value). */
    double destination_fuel_value;          /* $/L, 0 = disabled */

    /* Existing segment override (deprecated, use weight_profile) */
    int num_segments;
    FWSegment *segments;
} FWRefuelProblem;
```

**Solver Changes**
1. Pre-compute fuel consumption between each pair of consecutive stations
2. Use these values in LP constraint generation
3. Existing `fw_calc_fuel_consumed()` updated to use new model
4. Modified objective for ending inventory valuation:

**Objective function:**
```
minimize: Σ(x[i] * price[i]) - y[final] * destination_fuel_value
```

Where `y[final]` is fuel remaining at destination. When `destination_fuel_value > 0`:
- Solver is credited for fuel remaining at end
- If last station is cheap relative to future price → fill up
- If last station is expensive relative to future price → buy minimum

When `destination_fuel_value = 0`, reduces to current behavior (minimize purchase cost only).

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
    expected_cost -= fuel * destination_fuel_value  # credit for remaining fuel
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
