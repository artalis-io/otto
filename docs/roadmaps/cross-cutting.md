# Cross-Cutting Concerns

These features span multiple components and require coordination across the platform.

## 6. Distance and Duration Estimation (Cross-Cutting)

### Overview

All components need fast, accurate distance/duration estimates. This is provided by Velo routing engine, but there are important caching and approximation strategies.

### Estimation Tiers

| Tier | Speed | Accuracy | Use Case |
|------|-------|----------|----------|
| **Haversine** | O(1) | Low (ignores roads) | Coarse filtering |
| **Geodesic approx** | O(1) | Medium | Quick feasibility |
| **H3 Grid Cache** | O(1) | High | Repeated queries |
| **Full Routing** | O(n log n) | Exact | Final scheduling |

### Haversine Formula

Fast great-circle distance:
```c
double haversine_distance(double lat1, double lon1, double lat2, double lon2) {
    double dlat = (lat2 - lat1) * DEG_TO_RAD;
    double dlon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dlat/2) * sin(dlat/2) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return EARTH_RADIUS_KM * c;
}
```

### Geodesic Approximation with Circuity Factor

Road distance ≈ Haversine distance × circuity factor (typically 1.2-1.4):
```c
double estimated_road_distance(double lat1, double lon1, double lat2, double lon2) {
    double straight = haversine_distance(lat1, lon1, lat2, lon2);
    return straight * CIRCUITY_FACTOR;  /* 1.3 typical for US */
}

double estimated_duration(double distance_km, double avg_speed_kmh) {
    return distance_km / avg_speed_kmh * 3600;  /* seconds */
}
```

### H3 Grid Caching

For repeated queries, cache distances between H3 hexagons:
```c
typedef struct {
    uint64_t from_h3;
    uint64_t to_h3;
    double distance_km;
    double duration_sec;
} H3DistanceEntry;

/* Cache keyed by (from_h3, to_h3) pair */
/* Use resolution 5-7 for good balance of accuracy/cache size */
```

### Integration with Velo

For exact routing, use Velo's routing engine:
```c
/* Full route computation */
VeloRoute route;
velo_find_route(ctx, from_lat, from_lon, to_lat, to_lon, &route);
double distance = route.total_distance;
double duration = route.total_duration;
```

---

## 7. Cost and Profit Calculations (Cross-Cutting)

### Overview

All optimization components need consistent cost/profit calculations. This section describes the shared cost model used by Arbor (node scoring), Sigma (plan selection), and Pulse (schedule costing).

### Cost Model Data Structure

```c
/* Accumulated metrics during search/simulation */
typedef struct {
    /* Distances (meters) */
    double distance_empty;          /* Empty (deadhead) miles */
    double distance_loaded;         /* Loaded miles */
    double distance_penalized;      /* Empty miles that incur penalty */

    /* Durations (seconds) */
    double duration_off_duty;       /* Off-duty time */
    double duration_driving_empty;  /* Driving empty */
    double duration_driving_loaded; /* Driving loaded */
    double duration_on_duty_other;  /* On-duty non-driving (loading, waiting) */

    /* Costs (cents) */
    double cost_driver;             /* Driver pay */
    double cost_fuel;               /* Fuel cost */
    double cost_insurance;          /* Per-mile insurance */
    double cost_tractor_lease;      /* Time-based tractor lease */
    double cost_tractor_mileage;    /* Per-mile tractor cost */
    double cost_trailer_lease;      /* Time-based trailer lease */
    double cost_trailer_mileage;    /* Per-mile trailer cost */
    double cost_deadhead_penalty;   /* Penalty for empty miles */
    double cost_total;              /* Sum of all costs */

    /* Revenue and Profit (cents) */
    double revenue;
    double profit;                  /* revenue - cost_total */

    /* Objective value (for ranking) */
    double value;
} CostMetrics;
```

### Cost Factors (Configurable)

```c
typedef struct {
    /* Driver costs */
    double driver_rate_per_second;  /* Base driver pay rate */

    /* Fuel costs */
    double fuel_price_per_liter;    /* Current fuel price */
    double fuel_consumption_empty;  /* L/km when empty */
    double fuel_consumption_loaded; /* L/km when loaded */

    /* Equipment costs */
    double tractor_lease_per_second;
    double tractor_mileage_per_km;
    double trailer_lease_per_second;
    double trailer_mileage_per_km;

    /* Operational costs */
    double insurance_per_km;
    double deadhead_penalty_per_km;

    /* Piecewise fuel model (optional) */
    double *weight_breakpoints;     /* kg thresholds */
    double *consumption_rates;      /* L/km at each weight */
    int num_pwl_segments;
} CostFactors;
```

### Search Goals (Objective Functions)

Different optimization modes:

| Goal | Objective | Use Case |
|------|-----------|----------|
| `GROSS_PROFIT` | revenue - all_costs | Absolute profit maximization |
| `PER_HOUR_GROSS_PROFIT` | gross_profit / total_hours | Efficiency focus |
| `PROFIT` | revenue - simplified_costs | Quick estimation |
| `PER_HOUR_PROFIT` | profit / total_hours | Rate-based comparison |
| `REVENUE` | revenue only | Cost-agnostic selection |
| `PER_HOUR_REVENUE` | revenue / total_hours | Revenue efficiency |

```c
typedef enum {
    GOAL_GROSS_PROFIT,
    GOAL_PER_HOUR_GROSS_PROFIT,
    GOAL_PROFIT,
    GOAL_PER_HOUR_PROFIT,
    GOAL_REVENUE,
    GOAL_PER_HOUR_REVENUE,
} SearchGoal;
```

### Cost Calculation During Search

**Per-Node Update (Arbor)**:
- Compute delta metrics from last node
- Add to cumulative totals
- Calculate `delta_value` for node scoring

**Full-Path Evaluation (Leaf)**:
- Recompute total costs from cumulative metrics
- Apply normalization (per-hour if configured)
- This is the final ranking value

### Piecewise Linear Fuel Consumption

Fuel consumption depends on vehicle weight:

```c
/* Convert weight to fuel consumption rate */
double get_fuel_rate(double weight_kg, const CostFactors *factors) {
    for (int i = 0; i < factors->num_pwl_segments - 1; i++) {
        if (weight_kg <= factors->weight_breakpoints[i+1]) {
            /* Linear interpolation within segment */
            double t = (weight_kg - factors->weight_breakpoints[i]) /
                       (factors->weight_breakpoints[i+1] - factors->weight_breakpoints[i]);
            return factors->consumption_rates[i] +
                   t * (factors->consumption_rates[i+1] - factors->consumption_rates[i]);
        }
    }
    return factors->consumption_rates[factors->num_pwl_segments - 1];
}
```

This allows accurate fuel cost estimation for trucks that consume more fuel when loaded heavily.

---

## 8. FuelWise Integration (Refueling in Search)

### Overview

FuelWise already provides LP-based refueling optimization. For integration with Arbor search, we need two approaches:
1. **Fast DP approximation**: For search pruning and lower bounds
2. **Exact MILP solution**: For final plan costing

### DP Strategy (Fast, Approximate)

A recursive divide-and-conquer algorithm for quick refueling cost estimation:

```
PROCEDURE DPRefuel(start, end, stations, fuel_state):
    IF can_reach(start, end, fuel_state):
        RETURN 0  # No refueling needed

    # Find cheapest station between start and end
    cheapest = find_cheapest_station(stations, start, end)

    # Recurse on subsegments
    cost1 = DPRefuel(start, cheapest, stations, fuel_state)
    fuel_at_cheapest = fuel_state - consumption(start, cheapest)

    # Buy enough fuel to reach end with min_fuel buffer
    fuel_needed = consumption(cheapest, end) + MIN_FUEL - fuel_at_cheapest
    fuel_to_buy = max(0, min(fuel_needed, TANK_CAPACITY - fuel_at_cheapest))
    refuel_cost = fuel_to_buy * cheapest.price

    cost2 = DPRefuel(cheapest, end, stations, fuel_at_cheapest + fuel_to_buy)

    RETURN cost1 + refuel_cost + cost2
```

**Characteristics**:
- O(n log n) for n stations
- Yields feasible (not necessarily optimal) solution
- Good for lower-bound estimation during search

### MILP Strategy (Exact, for Final Costing)

The full FuelWise formulation (already implemented in `fw_refuel.c`).

**When to use**:
- Final plan evaluation
- When cost differences between top solutions are small
- When exact fuel cost is needed for billing/reporting

### Future Fuel Price Optionality

Account for remaining fuel capacity after route:
```c
/* Virtual future purchase at estimated price */
double future_price_per_gallon;  /* Expected price at end location */
/* Objective includes: fuel_purchased * price + remaining_capacity * future_price */
```

This lets the optimizer decide whether to buy more now (cheaper) or less now (leaving capacity for potentially cheaper future fuel).

### Piecewise Fuel Consumption

Already supported in FuelWise. Weight-dependent consumption:
- Segment weight from load profile
- PWL function maps weight → consumption rate
- Accurate fuel cost for loaded vs empty segments

---

