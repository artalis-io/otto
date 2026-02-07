# Atlas - Network Design Engine

### Overview

**ATLAS** (**A**llocation and **T**actical **L**ane **A**nalysis **S**ystem) provides network-level planning for lane balancing and capacity management.

### Core Problem

Fleet networks become imbalanced over time:
- **Headhaul/backhaul imbalance**: Too much freight going one direction
- **Deadhead accumulation**: Trucks repositioning empty
- **Capacity mismatch**: Wrong trucks in wrong places
- **Network smoothing**: New loads don't fit existing flow

### Key Distinction

Atlas operates at the **network level**, not the **truck level**:
- Does NOT assign specific trucks to specific loads
- Plans optimal lane flow and capacity allocation
- Guides which loads to accept/reject
- Identifies network imbalances before they become problems

### Lane Balancing Model

```c
typedef struct {
    uint32_t origin_region;
    uint32_t destination_region;
    double volume_forecast;    /* Expected loads per week */
    double current_capacity;   /* Available trucks per week */
    double rate_per_mile;      /* Average lane rate */
} LaneFlow;

typedef struct {
    LaneFlow *lanes;
    size_t lane_count;
    double imbalance_penalty;  /* Cost of imbalanced flow */
    double deadhead_cost_per_mile;
} NetworkState;

typedef struct {
    uint32_t region_id;
    double capacity_surplus;   /* Positive = excess trucks, negative = shortage */
    double recommended_rate_adjustment;  /* % rate change to balance */
} RegionBalance;

typedef struct {
    RegionBalance *regions;
    size_t region_count;
    double total_deadhead_forecast;
    double network_efficiency_score;  /* 0-1, 1 = perfectly balanced */
} NetworkAnalysis;

NetworkAnalysis at_analyze_network(const NetworkState *state);
```

### Tender Acceptance Scoring

When a new load is tendered, Atlas scores network fit:

```c
typedef struct {
    uint32_t origin_region;
    uint32_t destination_region;
    double load_volume;        /* Truckloads */
    double offered_rate;
} TenderRequest;

typedef struct {
    double network_fit_score;  /* 0-1, how well it fits network */
    double rate_vs_market;     /* Offered rate vs market (1.0 = at market) */
    double capacity_impact;    /* Effect on regional balance */
    bool accept_recommended;
    char *reasoning;           /* Human-readable explanation */
} TenderScore;

TenderScore at_score_tender(const NetworkState *state, const TenderRequest *tender);
```

### Capacity Planning

```c
typedef struct {
    uint32_t region_id;
    double trucks_needed;      /* Additional trucks needed */
    double trucks_excess;      /* Trucks that could be relocated */
    uint32_t *relocate_to;     /* Recommended destination regions */
    size_t relocate_count;
} CapacityRecommendation;

CapacityRecommendation *at_plan_capacity(
    const NetworkState *state,
    size_t *recommendation_count
);
```

### TODOs

- [ ] Define region/lane data structures
- [ ] Implement flow balance calculator
- [ ] Add headhaul/backhaul ratio metrics
- [ ] Tender acceptance scoring
- [ ] Capacity planning recommendations
- [ ] Integration with historical load data
- [ ] Seasonal pattern recognition
- [ ] Rate adjustment recommendations
- [ ] Network visualization API

---

## Component Summary

Current and planned components:

| Component | Status | Purpose |
|-----------|--------|---------|
| `ralph/` | Active | LP/MIP solver engine |
| `fuelwise/` | Active | Refueling optimization |
| `velo/` | Active | Routing engine |
| `carta/` | Active | Map tile generation |
| `shared/` | Active | Common geo utilities |
| `hose/` | **Planned** | Hours of Service rule engine |
| `tempo/` | **Planned** | Business rules / time window engine |
| `arbor/` | **Planned** | State-space search framework |
| `sigma/` | **Planned** | Fleet-wide plan selection (set covering MIP) |
| `pulse/` | **Planned** | Execution tracking and PTA computation |
| `nexus/` | **Planned** | External data integration gateway (TMS/ELD/LoadBoard) |
| `forge/` | **Planned** | Async job queue and worker orchestration |
| `quota/` | **Planned** | Rate quoting and pricing engine |
| `atlas/` | **Planned** | Network design and lane balancing |
| `api/` | Active | REST API server |
| `ui/` | Active | React frontend |

## Implementation Priority

1. **Project Renaming to OTTO** - Low priority (cosmetic, do when convenient)
2. **Forge Core** - High priority (enables async job execution for all modules)
3. **HoSE Core** - High priority (essential for realistic trucking optimization)
4. **Tempo Core** - High priority (time windows needed for realistic planning)
5. **Pulse Core** - High priority (PTA computation needed everywhere)
6. **Arbor Core** - Medium priority (enables advanced optimization)
7. **Sigma Core** - Medium priority (fleet-wide optimization, depends on Arbor)
8. **Nexus Core** - Medium priority (data integration, enables real-world deployment)
9. **Quota Core** - Medium priority (pricing/quoting, revenue generation)
10. **Atlas Core** - Medium priority (network design, lane balancing)
11. **Component Integration** - High priority (all modules working together)

---

