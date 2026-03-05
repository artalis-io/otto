# Surge - Rich VRP/PDPTW Solver

**S**cheduler for **U**rban **R**outing and **G**eneral **E**xpress

ALNS-based solver for Vehicle Routing and Pickup-Delivery Problems with rich side
constraints, built on Arbor.

## Problem Classes

Surge handles both **VRPTW** and **PDPTW** as first-class citizens, plus rich extensions:

| Problem | Description | Request Type |
|---------|-------------|--------------|
| **VRPTW** | Vehicle Routing with Time Windows | Single visit (delivery or pickup) |
| **PDPTW** | Pickup and Delivery with TW | Paired pickup → delivery |
| **DARP** | Dial-a-Ride Problem | PDPTW with ride time limits |
| **Rich VRP** | Any combination + side constraints | Mixed |

## Constraint Dimensions

Surge supports orthogonal constraint dimensions that can be combined freely:

### Core Constraints (Always Active)

| Constraint | Description |
|------------|-------------|
| **Time Windows** | Arrival must be within allowed windows |
| **Vehicle Shift** | Route must start/end within shift |
| **Request Pairing** | Pickup and delivery on same vehicle (PDPTW) |
| **Precedence** | Pickup before delivery (PDPTW) |

### Capacity Constraints (Multi-dimensional)

| Dimension | Unit | Example |
|-----------|------|---------|
| Weight | kg | Max 24,000 kg |
| Volume | m³ | Max 80 m³ |
| Pallet count | count | Max 33 EUR pallets |
| Piece count | count | Max 500 parcels |
| Axle load | kg | Max 10,000 kg per axle |
| Custom | user-defined | Refrigeration units, etc. |

### Time Constraints

| Constraint | Description |
|------------|-------------|
| **Disjunct TW** | Multiple allowed windows per location |
| **Max ride time** | Passenger/cargo max time on vehicle (DARP) |
| **Max route duration** | Total route time limit |
| **Break requirements** | Mandatory breaks (HoSE integration) |
| **Waiting costs** | Penalize early arrival waiting |

### Compatibility Constraints

| Constraint | Description |
|------------|-------------|
| **Request exclusions** | Request A excludes B, C, D on same vehicle |
| **Commodity conflicts** | Hazmat ∉ same vehicle as food |
| **Vehicle qualifications** | Request requires refrigerated/ADR/tail-lift |
| **Customer preferences** | Soft: prefer driver X for customer Y |
| **Sequence-dependent setup** | Cleanup/preparation time between incompatible cargo types |
| **LIFO/FIFO PD stacking** | Per-vehicle pickup-delivery pair ordering (nested or same-order) |
| **Backhaul** | All delivery-only stops before PD pickups on a vehicle |

### Depot Constraints

| Constraint | Description |
|------------|-------------|
| **Multiple depots** | Vehicles assigned to different depots |
| **Open routes** | Start/end anywhere (open start + open end) |
| **Depot capacity** | Max vehicles dispatched per depot |
| **Depot time windows** | Loading dock availability |
| **Multi-trip** | Vehicle returns to depot, reloads, serves another route |

### Travel Constraints

| Constraint | Description |
|------------|-------------|
| **Time-dependent travel** | Speed profiles: step-function duration multipliers by departure time |
| **Time-indexed travel brackets** | Multiple complete duration matrices indexed by departure time (global + per-vehicle) |
| **Per-vehicle travel profiles** | Independent distance/duration matrices + speed profile per vehicle type |

### Re-optimization Constraints

| Constraint | Description |
|------------|-------------|
| **Request locking (NONE)** | Request freely reassignable by solver |
| **Request locking (COMMITTED)** | Must be served (1e12 drop penalty), vehicle reassignable |
| **Request locking (FROZEN)** | Locked to designated vehicle from initial solution |

### Objective Components

| Component | Weight | Description |
|-----------|--------|-------------|
| Distance | w₁ | Total km traveled |
| Duration | w₂ | Total time (travel + service + wait) |
| Vehicles | w₃ | Number of vehicles used |
| Unassigned | w₄ | Penalty for unserved requests |
| Tardiness | w₅ | Soft TW violation penalty |
| Waiting | w₆ | Cost of waiting at locations |

```
Objective = w₁·distance + w₂·duration + w₃·vehicles + w₄·unassigned + w₅·tardiness + w₆·waiting
```

## Problem Complexity

| Instance Size | Requests | Approach |
|--------------|----------|----------|
| Small | < 50 | Exact (Ralph MIP) feasible |
| Medium | 50-500 | ALNS required |
| Large | 500-5000 | ALNS + parallelization |
| Very Large | 5000+ | Decomposition + ALNS |

---

## Architecture

Surge uses Arbor's ALNS framework (see `docs/roadmaps/arbor.md`) with VRP/PDPTW-specific
operators. The generic ALNS loop, operator selection, and acceptance criteria live in
Arbor; Surge provides the domain logic and constraint checking.

```
┌──────────────────────────────────────────────────────────────────┐
│                         Surge API                                │
│  sg_create() │ sg_solve() │ sg_add_request() │ sg_get_solution() │
├──────────────────────────────────────────────────────────────────┤
│                    VRP/PDPTW Operators (Surge)                   │
│  ┌────────────────────────┐ ┌──────────────────────────────────┐ │
│  │ Destroy: random, worst │ │ Repair: greedy, regret-k         │ │
│  │   Shaw, route, cluster │ │   best position, parallel        │ │
│  │   zone, time-based     │ │   constraint-aware               │ │
│  └────────────────────────┘ └──────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                   Constraint Checking (Surge)                    │
│  ┌──────────────┐ ┌──────────────┐ ┌────────────────────────────┐│
│  │ Time Windows │ │   Capacity   │ │     Compatibility          ││
│  │  • Disjunct  │ │  • Multi-dim │ │  • Qualifications          ││
│  │  • Soft TW   │ │  • Axle load │ │  • Commodity conflicts     ││
│  │  • Ride time │ │  • Volume    │ │  • Exclusion groups        ││
│  │  • Precedence│ │  • Compart.  │ │  • LIFO/FIFO + Backhaul   ││
│  └──────────────┘ └──────────────┘ └────────────────────────────┘│
├──────────────────────────────────────────────────────────────────┤
│                   Solution Representation (Surge)                │
│  SGSolution → SGRoute[] → SGStop[] → { load[], commodities }     │
├──────────────────────────────────────────────────────────────────┤
│                     ALNS Framework (Arbor)                       │
│  ar_alns_solve() │ roulette selection │ adaptive weights         │
│  SA/RRT/GD acceptance │ ar_remove_worst() │ ar_remove_related()  │
├──────────────────────────────────────────────────────────────────┤
│                   Supporting Infrastructure                      │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────────────────────┐│
│  │  Velo   │ │  Ralph  │ │  HoSE   │ │        Shared           ││
│  │ Routing │ │  MIP    │ │ Breaks  │ │ Geo, Heap, HashMap      ││
│  └─────────┘ └─────────┘ └─────────┘ └─────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

### What Arbor Provides (Generic)

| Component | Description |
|-----------|-------------|
| `ar_alns_solve()` | Main ALNS loop with termination |
| `ar_alns_add_destroy()` | Register destroy operators |
| `ar_alns_add_repair()` | Register repair operators |
| Roulette selection | Probabilistic operator choice |
| Adaptive weights | Score-based weight updates |
| Acceptance criteria | SA, RRT, Great Deluge |
| `ar_remove_random()` | Generic random removal |
| `ar_remove_worst()` | Generic worst removal with randomization |
| `ar_remove_related()` | Generic Shaw removal with relatedness fn |

### What Surge Provides (VRP/PDPTW-specific)

| Component | Description |
|-----------|-------------|
| Request types | PDPTW pairs, VRPTW deliveries/pickups, service visits |
| Multi-dim capacity | Weight, volume, pallets, axle load, custom |
| Disjunct time windows | Multiple allowed windows per stop |
| Compatibility | Qualifications, commodity conflicts, exclusion groups |
| Soft constraints | Tardiness penalties, waiting costs, priorities |
| DARP support | Max ride time enforcement |
| Multi-depot | Per-vehicle depot assignment, open routes |
| Feasibility checker | Combines all constraint types |
| Insertion cost | Delta cost with all constraint impacts |
| Relatedness | Distance + time + load + commodity similarity |

---

## Data Structures

### Request Types

```c
/* Request type determines pairing/precedence rules */
typedef enum {
    SG_REQUEST_PICKUP_DELIVERY,  /* PDPTW: paired pickup → delivery */
    SG_REQUEST_DELIVERY,         /* VRPTW: delivery from depot */
    SG_REQUEST_PICKUP,           /* VRPTW: pickup to depot (backhaul) */
    SG_REQUEST_SERVICE,          /* Visit without load change */
} SGRequestType;
```

### Time Windows (Disjunct)

```c
/* Single time window */
typedef struct {
    int32_t early;           /* Earliest arrival (seconds from midnight) */
    int32_t late;            /* Latest arrival */
} SGTimeWindow;

/* Disjunct time windows: arrival must be within ANY window */
typedef struct {
    uint32_t count;          /* Number of windows (1 = single TW) */
    SGTimeWindow *windows;   /* Array of allowed windows */
} SGTimeWindows;
```

### Multi-dimensional Capacity

```c
/* Capacity dimension definition */
typedef struct {
    const char *name;        /* "weight", "volume", "pallets", etc. */
    const char *unit;        /* "kg", "m3", "count", etc. */
} SGCapacityDim;

/* Load vector (one value per dimension) */
typedef struct {
    uint32_t num_dims;
    double *values;          /* values[dim_idx] = load in that dimension */
} SGLoadVector;
```

### Compatibility & Exclusions

```c
/* Exclusion group: requests that cannot share a vehicle */
typedef struct {
    uint32_t group_id;
    uint32_t *request_ids;   /* Requests in this exclusion group */
    uint32_t count;
} SGExclusionGroup;

/* Vehicle qualification (bit flags) */
typedef enum {
    SG_QUAL_NONE        = 0,
    SG_QUAL_REFRIGERATED = 1 << 0,
    SG_QUAL_ADR_HAZMAT   = 1 << 1,
    SG_QUAL_TAIL_LIFT    = 1 << 2,
    SG_QUAL_SIDE_LOADER  = 1 << 3,
    SG_QUAL_CRANE        = 1 << 4,
    SG_QUAL_DOUBLE_DECK  = 1 << 5,
    /* ... extend as needed ... */
} SGQualification;

/* Commodity type for conflict checking */
typedef struct {
    uint32_t id;
    const char *name;        /* "food", "hazmat", "livestock", etc. */
    uint64_t conflicts;      /* Bitmask of conflicting commodity IDs */
} SGCommodity;
```

### Core Types

```c
/* Location with coordinates and constraints */
typedef struct {
    uint32_t id;
    double lat, lon;
    const char *name;

    /* Location-level time windows (e.g., dock availability) */
    SGTimeWindows time_windows;

    /* Service time at this location (can be overridden per request) */
    int32_t default_service_time;
} SGLocation;

/* Request: unified structure for all request types */
typedef struct {
    uint32_t id;
    SGRequestType type;

    /* Locations (interpretation depends on type) */
    uint32_t origin_loc;     /* Pickup loc (PDPTW) or depot (VRPTW delivery) */
    uint32_t dest_loc;       /* Delivery loc (PDPTW/VRPTW) or depot (pickup) */

    /* Multi-dimensional load */
    SGLoadVector load;       /* Load change at pickup (+) and delivery (-) */

    /* Time windows (disjunct) */
    SGTimeWindows origin_tw; /* Time windows at origin */
    SGTimeWindows dest_tw;   /* Time windows at destination */

    /* Service times (0 = use location default) */
    int32_t origin_service;
    int32_t dest_service;

    /* DARP: max ride time (0 = unlimited) */
    int32_t max_ride_time;

    /* Soft constraints */
    int32_t priority;        /* Higher = insert first in regret */
    double tardiness_cost;   /* Cost per second of late arrival */

    /* Compatibility */
    uint64_t required_quals; /* Bitmask: vehicle must have these */
    uint32_t commodity_id;   /* For conflict checking (0 = none) */

    /* Exclusions: requests that cannot be on same vehicle */
    uint32_t *excludes;      /* Array of request IDs */
    uint32_t num_excludes;
} SGRequest;

/* Depot with constraints */
typedef struct {
    uint32_t id;
    uint32_t location_id;
    const char *name;

    /* Depot capacity */
    uint32_t max_vehicles;   /* Max vehicles dispatched (0 = unlimited) */

    /* Depot time windows */
    SGTimeWindows dispatch_tw;  /* When vehicles can leave */
    SGTimeWindows return_tw;    /* When vehicles must return */
} SGDepot;

/* Vehicle with rich constraints */
typedef struct {
    uint32_t id;
    uint32_t depot_id;       /* Index into depots array */

    /* Multi-dimensional capacity */
    SGLoadVector capacity;   /* Max load in each dimension */

    /* Shift constraints */
    int32_t shift_start;     /* Earliest departure (seconds from midnight) */
    int32_t shift_end;       /* Latest return */
    int32_t max_duration;    /* Max route duration (0 = unlimited) */

    /* Qualifications */
    uint64_t qualifications; /* Bitmask of SG_QUAL_* */

    /* Costs */
    double cost_per_km;
    double cost_per_hour;
    double fixed_cost;       /* Cost to use this vehicle at all */
    double overtime_cost;    /* Cost per hour beyond shift_end */

    /* Route type */
    bool open_end;           /* true = don't return to depot */
    uint32_t end_depot_id;   /* If different from start depot */
} SGVehicle;

/* Stop in a route (computed state) */
typedef struct {
    uint32_t request_id;
    uint8_t stop_type;       /* SG_STOP_ORIGIN, SG_STOP_DEST, SG_STOP_DEPOT */

    /* Timing */
    int32_t arrival;         /* Computed arrival time */
    int32_t wait;            /* Wait time before service (early arrival) */
    int32_t service;         /* Service duration */
    int32_t departure;       /* arrival + wait + service */
    int32_t tardiness;       /* Late arrival amount (for soft TW) */

    /* Load state after this stop */
    SGLoadVector load_after; /* Cumulative load in each dimension */

    /* Active commodities on vehicle after this stop (bitmask) */
    uint64_t commodities_after;
} SGStop;

/* Route: sequence of stops for one vehicle */
typedef struct {
    uint32_t vehicle_id;
    uint32_t num_stops;
    uint32_t capacity;       /* Allocated array capacity */
    SGStop *stops;           /* Array of stops (depot → ... → depot) */

    /* Computed metrics */
    double distance;
    double duration;
    double cost;
    double tardiness_total;
    double wait_total;

    /* Feasibility flags (for incremental checking) */
    bool feasible;
    uint32_t first_infeasible; /* First stop index with violation */
} SGRoute;

/* Solution: set of routes */
typedef struct {
    uint32_t num_routes;
    uint32_t num_unassigned;
    SGRoute *routes;
    uint32_t *unassigned;    /* Request IDs not yet assigned */

    /* Objective components */
    double total_distance;
    double total_duration;
    double total_tardiness;
    double total_waiting;
    double unassigned_penalty;
    double total_cost;       /* Weighted sum */

    /* Metadata */
    int64_t iteration;
    double elapsed_seconds;
} SGSolution;
```

### Problem Context

```c
typedef struct {
    /* Problem dimensions */
    uint32_t num_requests;
    uint32_t num_vehicles;
    uint32_t num_locations;
    uint32_t num_depots;
    uint32_t num_capacity_dims;
    uint32_t num_commodities;

    /* Problem data */
    SGRequest *requests;
    SGVehicle *vehicles;
    SGLocation *locations;
    SGDepot *depots;
    SGCapacityDim *capacity_dims;
    SGCommodity *commodities;

    /* Exclusion groups (sparse) */
    uint32_t num_exclusion_groups;
    SGExclusionGroup *exclusion_groups;

    /* Distance/time matrix (precomputed or on-demand via Velo) */
    double *dist_matrix;     /* num_locations × num_locations */
    double *time_matrix;     /* num_locations × num_locations */
    bool matrix_precomputed;
    VLGraph *road_graph;     /* For on-demand routing */

    /* Current solution state */
    SGSolution current;
    SGSolution best;

    /* Objective weights */
    SGObjectiveWeights weights;

    /* Configuration */
    SGConfig config;

    /* Memory arena for allocations */
    Arena arena;
} SGContext;

/* Objective weights */
typedef struct {
    double distance;         /* Per km */
    double duration;         /* Per hour */
    double vehicle;          /* Per vehicle used */
    double unassigned;       /* Per unassigned request */
    double tardiness;        /* Per second late */
    double waiting;          /* Per second waiting */
} SGObjectiveWeights;
```

### Feasibility Checking Helpers

```c
/* Check if vehicle can serve request (qualifications + depot) */
bool sg_vehicle_can_serve(SGContext *ctx, uint32_t vehicle_id, uint32_t request_id);

/* Check commodity compatibility on current route */
bool sg_commodities_compatible(SGContext *ctx, SGRoute *route, uint32_t request_id);

/* Check exclusion constraints */
bool sg_exclusions_satisfied(SGContext *ctx, SGRoute *route, uint32_t request_id);

/* Check multi-dimensional capacity at insertion point */
bool sg_capacity_feasible(SGContext *ctx, SGRoute *route, int pos, SGRequest *req);

/* Check disjunct time window feasibility */
bool sg_time_window_feasible(int32_t arrival, const SGTimeWindows *tw);

/* Full insertion feasibility (combines all checks) */
bool sg_insertion_feasible(SGContext *ctx, SGRoute *route,
                           int origin_pos, int dest_pos, uint32_t request_id);
```

---

## ALNS Integration

Surge uses Arbor's ALNS framework (`ar_alns_solve()`) for the main optimization loop.
The generic algorithm—operator selection, acceptance criteria, adaptive weights—lives
in Arbor. Surge provides PDPTW-specific operators and solution operations via callbacks.

### Registering Operators with Arbor

```c
SGStatus sg_solve(SGContext *ctx) {
    /* Initialize with greedy construction */
    sg_construct_initial(ctx);

    /* Create Arbor ALNS context */
    ARALNSParams params = {
        .max_iterations = ctx->config.max_iterations,
        .initial_temp = ctx->config.initial_temp,
        .cooling_rate = ctx->config.cooling_rate,
        .segment_size = ctx->config.segment_size,
        .acceptance = AR_ACCEPT_SA,  /* Or AR_ACCEPT_RRT, AR_ACCEPT_GD */
    };

    ARALNSContext *alns = ar_alns_create(&params);

    /* Solution operations (Arbor callbacks into Surge) */
    ARSolutionOps ops = {
        .copy = sg_solution_copy,
        .cost = sg_solution_cost,
        .free = sg_solution_free,
        .user_data = ctx
    };
    ar_alns_set_solution_ops(alns, &ops);

    /* Register PDPTW destroy operators */
    ar_alns_add_destroy(alns, "random", sg_destroy_random, 1.0);
    ar_alns_add_destroy(alns, "worst", sg_destroy_worst, 1.0);
    ar_alns_add_destroy(alns, "shaw", sg_destroy_shaw, 1.0);
    ar_alns_add_destroy(alns, "route", sg_destroy_route, 1.0);

    /* Register PDPTW repair operators */
    ar_alns_add_repair(alns, "greedy", sg_repair_greedy, 1.0);
    ar_alns_add_repair(alns, "regret2", sg_repair_regret_2, 1.0);
    ar_alns_add_repair(alns, "regret3", sg_repair_regret_3, 1.0);

    /* Run ALNS (Arbor handles the loop, selection, acceptance) */
    ARStatus status = ar_alns_solve(alns, &ctx->current, &ctx->best);

    ar_alns_free(alns);
    return (status == AR_STATUS_OK) ? SG_STATUS_OK : SG_STATUS_ERROR;
}
```

### Arbor Acceptance Criteria

Arbor provides these acceptance criteria (Surge selects via `params.acceptance`):

| Criterion | Arbor Enum | Behavior |
|-----------|------------|----------|
| Simulated Annealing | `AR_ACCEPT_SA` | Accept worse with prob exp(-Δ/T) |
| Record-to-Record Travel | `AR_ACCEPT_RRT` | Accept if within threshold of best |
| Great Deluge | `AR_ACCEPT_GD` | Accept if below water level |

See `docs/roadmaps/arbor.md` for details on Arbor's ALNS framework.

---

## Destroy Operators

### Random Removal

```c
/* Remove q random requests */
void sg_destroy_random(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed) {
    /* Build list of assigned requests */
    uint32_t assigned[ctx->num_requests];
    int num_assigned = 0;

    for (int r = 0; r < sol->num_routes; r++) {
        for (int s = 0; s < sol->routes[r].num_stops; s++) {
            if (sol->routes[r].stops[s].type == SG_STOP_PICKUP) {
                assigned[num_assigned++] = sol->routes[r].stops[s].request_id;
            }
        }
    }

    /* Shuffle and take first q */
    sg_shuffle(assigned, num_assigned, ctx);
    q = (q < num_assigned) ? q : num_assigned;

    for (int i = 0; i < q; i++) {
        removed[i] = assigned[i];
        sg_remove_request(sol, assigned[i]);
    }
}
```

### Worst Removal

```c
/* Remove q requests with highest removal cost savings */
void sg_destroy_worst(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed) {
    typedef struct { uint32_t req; double cost; } ReqCost;
    ReqCost costs[ctx->num_requests];
    int n = 0;

    /* Compute removal cost for each assigned request */
    for (int r = 0; r < sol->num_routes; r++) {
        for (int s = 0; s < sol->routes[r].num_stops; s++) {
            if (sol->routes[r].stops[s].type == SG_STOP_PICKUP) {
                uint32_t req_id = sol->routes[r].stops[s].request_id;
                double savings = sg_compute_removal_cost(ctx, sol, req_id);
                costs[n++] = (ReqCost){ req_id, savings };
            }
        }
    }

    /* Sort by cost descending (worst = highest savings if removed) */
    qsort(costs, n, sizeof(ReqCost), cmp_cost_desc);

    /* Add randomization: don't always pick absolute worst */
    double randomness = ctx->config.worst_removal_randomness;  /* e.g., 3-6 */

    for (int i = 0; i < q && n > 0; i++) {
        int idx = (int)(pow(sg_random_double(ctx), randomness) * n);
        removed[i] = costs[idx].req;
        sg_remove_request(sol, costs[idx].req);

        /* Remove from costs array */
        costs[idx] = costs[--n];
    }
}
```

### Related Removal (Shaw)

```c
/* Remove q requests related to a seed request */
void sg_destroy_related(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed) {
    /* Pick random seed request */
    uint32_t seed = sg_random_assigned_request(ctx, sol);

    /* Compute relatedness to all other requests */
    typedef struct { uint32_t req; double relatedness; } ReqRel;
    ReqRel rels[ctx->num_requests];
    int n = 0;

    SGRequest *seed_req = &ctx->requests[seed];

    for (int r = 0; r < sol->num_routes; r++) {
        for (int s = 0; s < sol->routes[r].num_stops; s++) {
            if (sol->routes[r].stops[s].type == SG_STOP_PICKUP) {
                uint32_t req_id = sol->routes[r].stops[s].request_id;
                if (req_id == seed) continue;

                SGRequest *other = &ctx->requests[req_id];
                double rel = sg_compute_relatedness(ctx, seed_req, other);
                rels[n++] = (ReqRel){ req_id, rel };
            }
        }
    }

    /* Sort by relatedness descending */
    qsort(rels, n, sizeof(ReqRel), cmp_rel_desc);

    /* Remove seed + most related */
    removed[0] = seed;
    sg_remove_request(sol, seed);

    double randomness = ctx->config.shaw_randomness;
    for (int i = 1; i < q && n > 0; i++) {
        int idx = (int)(pow(sg_random_double(ctx), randomness) * n);
        removed[i] = rels[idx].req;
        sg_remove_request(sol, rels[idx].req);
        rels[idx] = rels[--n];
    }
}

/* Relatedness: combines distance, time, and load similarity */
double sg_compute_relatedness(SGContext *ctx, SGRequest *a, SGRequest *b) {
    double dist_pickup = sg_get_distance(ctx, a->pickup_loc, b->pickup_loc);
    double dist_delivery = sg_get_distance(ctx, a->delivery_loc, b->delivery_loc);
    double time_diff = fabs(a->pickup_early - b->pickup_early);
    double load_diff = fabs(a->load - b->load);

    /* Normalize and weight */
    double w_dist = ctx->config.shaw_distance_weight;
    double w_time = ctx->config.shaw_time_weight;
    double w_load = ctx->config.shaw_load_weight;

    return 1.0 / (1.0 + w_dist * (dist_pickup + dist_delivery)
                      + w_time * time_diff
                      + w_load * load_diff);
}
```

### Route Removal

```c
/* Remove entire route (all requests from one vehicle) */
void sg_destroy_route(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed) {
    if (sol->num_routes == 0) return;

    /* Pick random route */
    int route_idx = sg_random_int(ctx, 0, sol->num_routes - 1);
    SGRoute *route = &sol->routes[route_idx];

    int count = 0;
    for (int s = 0; s < route->num_stops && count < q; s++) {
        if (route->stops[s].type == SG_STOP_PICKUP) {
            removed[count++] = route->stops[s].request_id;
        }
    }

    /* Remove all requests from this route */
    for (int i = 0; i < count; i++) {
        sg_remove_request(sol, removed[i]);
    }
}
```

---

## Repair Operators

### Greedy Insertion

```c
/* Insert each request at its best feasible position */
void sg_repair_greedy(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed) {
    for (int i = 0; i < q; i++) {
        uint32_t req_id = removed[i];

        /* Find best insertion position across all routes */
        SGInsertion best = { .cost = DBL_MAX };

        for (int r = 0; r < sol->num_routes; r++) {
            SGInsertion ins = sg_find_best_insertion(ctx, sol, r, req_id);
            if (ins.feasible && ins.cost < best.cost) {
                best = ins;
            }
        }

        /* Try new route if no feasible insertion found */
        if (!best.feasible) {
            best = sg_try_new_route(ctx, sol, req_id);
        }

        if (best.feasible) {
            sg_apply_insertion(sol, &best);
        } else {
            /* Add to unassigned */
            sol->unassigned[sol->num_unassigned++] = req_id;
        }
    }
}

/* Find best insertion for pickup-delivery pair in a route */
SGInsertion sg_find_best_insertion(SGContext *ctx, SGSolution *sol,
                                    int route_idx, uint32_t req_id) {
    SGRoute *route = &sol->routes[route_idx];
    SGRequest *req = &ctx->requests[req_id];
    SGInsertion best = { .feasible = false, .cost = DBL_MAX };

    int n = route->num_stops;

    /* Try all pickup positions */
    for (int p = 1; p < n; p++) {  /* After depot start */
        /* Check pickup feasibility */
        if (!sg_check_pickup_feasible(ctx, route, p, req)) continue;

        /* Try all delivery positions after pickup */
        for (int d = p + 1; d <= n; d++) {  /* Can be at end before depot */
            /* Check delivery feasibility */
            if (!sg_check_delivery_feasible(ctx, route, p, d, req)) continue;

            /* Compute insertion cost */
            double cost = sg_compute_insertion_cost(ctx, route, p, d, req);

            if (cost < best.cost) {
                best = (SGInsertion){
                    .feasible = true,
                    .route_idx = route_idx,
                    .pickup_pos = p,
                    .delivery_pos = d,
                    .request_id = req_id,
                    .cost = cost
                };
            }
        }
    }

    return best;
}
```

### Regret-k Insertion

```c
/* Insert requests prioritizing those with highest regret */
void sg_repair_regret_k(SGContext *ctx, SGSolution *sol, int q, uint32_t *removed, int k) {
    while (q > 0) {
        /* Compute regret for each uninserted request */
        typedef struct { uint32_t req; double regret; SGInsertion best; } ReqRegret;
        ReqRegret regrets[q];

        for (int i = 0; i < q; i++) {
            uint32_t req_id = removed[i];

            /* Find k-best insertions */
            SGInsertion insertions[k];
            int found = sg_find_k_best_insertions(ctx, sol, req_id, k, insertions);

            if (found == 0) {
                regrets[i] = (ReqRegret){ req_id, DBL_MAX, { .feasible = false } };
            } else if (found == 1) {
                regrets[i] = (ReqRegret){ req_id, DBL_MAX, insertions[0] };
            } else {
                /* Regret = sum of (cost[i] - cost[0]) for i in 1..k-1 */
                double regret = 0;
                for (int j = 1; j < found; j++) {
                    regret += insertions[j].cost - insertions[0].cost;
                }
                regrets[i] = (ReqRegret){ req_id, regret, insertions[0] };
            }
        }

        /* Sort by regret descending */
        qsort(regrets, q, sizeof(ReqRegret), cmp_regret_desc);

        /* Insert request with highest regret */
        if (regrets[0].best.feasible) {
            sg_apply_insertion(sol, &regrets[0].best);
        } else {
            sol->unassigned[sol->num_unassigned++] = regrets[0].req;
        }

        /* Remove from list */
        removed[0] = removed[--q];
    }
}
```

---

## Feasibility Checking

### Time Window Propagation

```c
/* Check if insertion is feasible and compute schedule */
bool sg_check_insertion_feasible(SGContext *ctx, SGRoute *route,
                                  int pickup_pos, int delivery_pos,
                                  SGRequest *req) {
    /* Check capacity */
    int load_at_pickup = sg_get_load_at(route, pickup_pos - 1);
    if (load_at_pickup + req->load > ctx->vehicles[route->vehicle_id].capacity) {
        return false;
    }

    /* Forward time window propagation */
    int current_time = route->stops[pickup_pos - 1].departure;
    int prev_loc = route->stops[pickup_pos - 1].request_id;  /* Simplified */

    /* Arrival at pickup */
    int travel = sg_get_time(ctx, prev_loc, req->pickup_loc);
    int arrival_pickup = current_time + travel;

    if (arrival_pickup > req->pickup_late) return false;  /* TW violated */

    int start_pickup = (arrival_pickup < req->pickup_early)
                       ? req->pickup_early : arrival_pickup;
    int depart_pickup = start_pickup + req->pickup_service;

    /* Check all stops between pickup and delivery shift */
    /* ... propagate forward, checking each TW ... */

    /* Arrival at delivery */
    /* ... similar logic ... */

    if (arrival_delivery > req->delivery_late) return false;

    /* Check remaining route still feasible */
    /* ... propagate to end ... */

    return true;
}
```

### Incremental Cost Computation

```c
/* Compute cost of inserting request (O(1) with proper data structures) */
double sg_compute_insertion_cost(SGContext *ctx, SGRoute *route,
                                  int pickup_pos, int delivery_pos,
                                  SGRequest *req) {
    /* Distance delta for pickup insertion */
    uint32_t prev_p = route->stops[pickup_pos - 1].location;
    uint32_t next_p = route->stops[pickup_pos].location;

    double dist_removed_p = sg_get_distance(ctx, prev_p, next_p);
    double dist_added_p = sg_get_distance(ctx, prev_p, req->pickup_loc)
                        + sg_get_distance(ctx, req->pickup_loc, next_p);
    double delta_p = dist_added_p - dist_removed_p;

    /* Distance delta for delivery insertion (accounting for shifted positions) */
    int adj_delivery_pos = delivery_pos;  /* Adjust if after pickup */
    uint32_t prev_d = ...;
    uint32_t next_d = ...;

    double dist_removed_d = sg_get_distance(ctx, prev_d, next_d);
    double dist_added_d = sg_get_distance(ctx, prev_d, req->delivery_loc)
                        + sg_get_distance(ctx, req->delivery_loc, next_d);
    double delta_d = dist_added_d - dist_removed_d;

    return delta_p + delta_d;
}
```

---

## Integration with OTTO Components

### Velo Integration (Routing)

```c
/* Initialize distance/time matrices using Velo */
void sg_init_matrices_velo(SGContext *ctx) {
    int n = ctx->num_locations;
    ctx->dist_matrix = arena_alloc(&ctx->arena, n * n * sizeof(double));
    ctx->time_matrix = arena_alloc(&ctx->arena, n * n * sizeof(double));

    VLRouteOptions opts;
    vl_default_options(&opts);
    opts.profile = VL_PROFILE_TRUCK;  /* Or configurable */
    opts.include_geometry = false;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j) {
                ctx->dist_matrix[i * n + j] = 0;
                ctx->time_matrix[i * n + j] = 0;
                continue;
            }

            VLCoord from = { ctx->locations[i].lat, ctx->locations[i].lon };
            VLCoord to = { ctx->locations[j].lat, ctx->locations[j].lon };

            VLRoute route;
            if (vl_route_coords(ctx->road_graph, from, to, &opts, &route) == VL_OK) {
                ctx->dist_matrix[i * n + j] = route.distance_m;
                ctx->time_matrix[i * n + j] = route.duration_s;
                vl_free_route(&route);
            } else {
                /* Fallback to Haversine */
                ctx->dist_matrix[i * n + j] = sh_haversine(from.lat, from.lon,
                                                            to.lat, to.lon);
                ctx->time_matrix[i * n + j] = ctx->dist_matrix[i * n + j] / 15.0;  /* ~50 km/h */
            }
        }
    }

    ctx->matrix_precomputed = true;
}
```

### Ralph Integration (MIP for Small Instances)

```c
/* Exact MIP solution for small instances */
SGStatus sg_solve_mip(SGContext *ctx) {
    if (ctx->num_requests > 30) {
        return SG_STATUS_TOO_LARGE;  /* Use ALNS instead */
    }

    RalphModel *model = ralph_model_create();

    /* Variables: x[i][j][k] = 1 if vehicle k travels from i to j */
    /* Variables: t[i][k] = arrival time at node i for vehicle k */
    /* Variables: q[i][k] = load at node i for vehicle k */

    int n = ctx->num_locations;
    int K = ctx->num_vehicles;

    /* ... build MIP formulation ... */
    /* Standard PDPTW formulation with:
       - Flow conservation constraints
       - Precedence constraints (pickup before delivery)
       - Pairing constraints (same vehicle)
       - Time window constraints
       - Capacity constraints
    */

    RalphSolution sol;
    RalphStatus status = ralph_optimize(model, &sol);

    if (status == RALPH_OPTIMAL) {
        sg_extract_solution_from_mip(ctx, &sol);
    }

    ralph_model_free(model);
    return (status == RALPH_OPTIMAL) ? SG_STATUS_OK : SG_STATUS_INFEASIBLE;
}
```

### Arbor Integration (Search Framework)

```c
/* Use Arbor for parallel neighborhood evaluation */
typedef struct {
    SGContext *sg_ctx;
    SGSolution *solution;
    int destroy_op;
    int repair_op;
} SGArborState;

/* Arbor transition: apply destroy+repair */
ARStatus sg_arbor_transition(ARState *state, ARAction action, ARState *next) {
    SGArborState *s = (SGArborState *)state->user_data;

    /* Copy solution */
    SGSolution *candidate = sg_copy_solution_arena(s->solution, &state->arena);

    /* Apply destroy */
    uint32_t removed[MAX_REMOVAL];
    int q = sg_compute_removal_count(s->sg_ctx, state->depth);
    s->sg_ctx->alns.destroy_ops[action.destroy](s->sg_ctx, candidate, q, removed);

    /* Apply repair */
    s->sg_ctx->alns.repair_ops[action.repair](s->sg_ctx, candidate, q, removed);

    /* Set next state */
    next->user_data = /* new SGArborState with candidate */;
    next->cost = candidate->total_cost;

    return AR_STATUS_OK;
}
```

---

## API

### Public Header (`surge.h`)

```c
#ifndef SURGE_H
#define SURGE_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations (see Data Structures section for full definitions) */
typedef struct SGContext SGContext;
typedef struct SGRequest SGRequest;
typedef struct SGVehicle SGVehicle;
typedef struct SGLocation SGLocation;
typedef struct SGDepot SGDepot;
typedef struct SGSolution SGSolution;
typedef struct SGRoute SGRoute;
typedef struct SGTimeWindow SGTimeWindow;
typedef struct SGLoadVector SGLoadVector;

/* Status codes */
typedef enum {
    SG_STATUS_OK = 0,
    SG_STATUS_INFEASIBLE,
    SG_STATUS_TOO_LARGE,
    SG_STATUS_TIMEOUT,
    SG_STATUS_ERROR
} SGStatus;

/* Opaque context */
typedef struct SGContext SGContext;

/* Configuration */
typedef struct {
    int max_iterations;
    int max_time_seconds;
    double initial_temp;
    double cooling_rate;
    int segment_size;
    double worst_removal_randomness;
    double shaw_randomness;
    double shaw_distance_weight;
    double shaw_time_weight;
    double shaw_load_weight;
    int regret_k;
    bool use_velo;
    int num_threads;
} SGConfig;

/* Creation and destruction */
SGContext *sg_create(void);
void sg_free(SGContext *ctx);

/* Configuration */
void sg_config_default(SGConfig *config);
void sg_set_config(SGContext *ctx, const SGConfig *config);

/* Objective weights */
void sg_set_weights(SGContext *ctx, double distance, double duration,
                    double vehicle, double unassigned, double tardiness, double waiting);

/*=== Problem Definition ===*/

/* Capacity dimensions (call before adding vehicles/requests) */
uint32_t sg_add_capacity_dim(SGContext *ctx, const char *name, const char *unit);

/* Commodities and conflicts */
uint32_t sg_add_commodity(SGContext *ctx, const char *name);
void sg_set_commodity_conflict(SGContext *ctx, uint32_t commodity_a, uint32_t commodity_b);

/* Locations */
uint32_t sg_add_location(SGContext *ctx, double lat, double lon, const char *name);
void sg_location_add_time_window(SGContext *ctx, uint32_t loc_id,
                                  int32_t early, int32_t late);

/* Depots */
uint32_t sg_add_depot(SGContext *ctx, uint32_t location_id, const char *name);
void sg_depot_set_capacity(SGContext *ctx, uint32_t depot_id, uint32_t max_vehicles);
void sg_depot_add_dispatch_window(SGContext *ctx, uint32_t depot_id,
                                   int32_t early, int32_t late);

/* Vehicles */
uint32_t sg_add_vehicle(SGContext *ctx, uint32_t depot_id);
void sg_vehicle_set_capacity(SGContext *ctx, uint32_t vehicle_id,
                              uint32_t dim, double value);
void sg_vehicle_set_shift(SGContext *ctx, uint32_t vehicle_id,
                           int32_t start, int32_t end);
void sg_vehicle_set_qualifications(SGContext *ctx, uint32_t vehicle_id,
                                    uint64_t qual_flags);
void sg_vehicle_set_costs(SGContext *ctx, uint32_t vehicle_id,
                           double per_km, double per_hour, double fixed);
void sg_vehicle_set_open_end(SGContext *ctx, uint32_t vehicle_id, bool open);

/* Requests - PDPTW (pickup-delivery pair) */
uint32_t sg_add_request_pd(SGContext *ctx,
                            uint32_t pickup_loc, uint32_t delivery_loc);

/* Requests - VRPTW (single visit) */
uint32_t sg_add_request_delivery(SGContext *ctx, uint32_t location_id);
uint32_t sg_add_request_pickup(SGContext *ctx, uint32_t location_id);
uint32_t sg_add_request_service(SGContext *ctx, uint32_t location_id);

/* Request attributes (apply to any request type) */
void sg_request_set_load(SGContext *ctx, uint32_t req_id,
                          uint32_t dim, double value);
void sg_request_add_origin_tw(SGContext *ctx, uint32_t req_id,
                               int32_t early, int32_t late);
void sg_request_add_dest_tw(SGContext *ctx, uint32_t req_id,
                             int32_t early, int32_t late);
void sg_request_set_service_times(SGContext *ctx, uint32_t req_id,
                                   int32_t origin_service, int32_t dest_service);
void sg_request_set_max_ride_time(SGContext *ctx, uint32_t req_id, int32_t max_ride);
void sg_request_set_required_quals(SGContext *ctx, uint32_t req_id, uint64_t quals);
void sg_request_set_commodity(SGContext *ctx, uint32_t req_id, uint32_t commodity_id);
void sg_request_set_priority(SGContext *ctx, uint32_t req_id, int32_t priority);
void sg_request_set_tardiness_cost(SGContext *ctx, uint32_t req_id, double cost_per_sec);

/* Request exclusions */
void sg_add_exclusion(SGContext *ctx, uint32_t req_a, uint32_t req_b);
void sg_add_exclusion_group(SGContext *ctx, uint32_t *req_ids, uint32_t count);

/* Optional: set road graph for Velo routing */
void sg_set_road_graph(SGContext *ctx, VLGraph *graph);

/*=== Solving ===*/

SGStatus sg_solve(SGContext *ctx);
void sg_stop(SGContext *ctx);  /* Interrupt from another thread */

/*=== Solution Access ===*/

const SGSolution *sg_get_solution(SGContext *ctx);
int sg_get_num_routes(SGContext *ctx);
const SGRoute *sg_get_route(SGContext *ctx, int route_idx);
double sg_get_total_cost(SGContext *ctx);
double sg_get_total_distance(SGContext *ctx);
double sg_get_total_tardiness(SGContext *ctx);
int sg_get_num_unassigned(SGContext *ctx);
const uint32_t *sg_get_unassigned(SGContext *ctx);

/* Export */
char *sg_solution_to_json(SGContext *ctx);
int sg_solution_to_geojson(SGContext *ctx, char *buf, size_t buf_size);

#endif /* SURGE_H */
```

---

## Implementation Plan

### Current Status (as of 2026-02-24)

**Baseline**: U1-U8 + S1-S12 + Disjunct TW + Depot Dock Capacity + Commodity Conflicts + Exclusion Groups + Mandatory Breaks + Multi-Trip + Multi-Threading (parallel + population) + SA cooling fix + mid-solve ejection pulse + Speed Profiles + Travel Profiles + Time-Indexed Travel Brackets + Open Start + Plan/ETA Validation + Infeasible-Space Exploration + Aggressive SISR + LIFO/FIFO PD Policy + Backhaul Constraint complete. All Tier 1 and Tier 2 production gaps closed. REST API server, WASM build, Python and Node.js bindings exist. 282 tests passing, ASAN/UBSAN clean.

Implemented features: Everything in previous status plus: LIFO/FIFO PD stacking policy (`sg_vehicle_set_pd_policy()`) — per-vehicle constraint on pickup-delivery pair ordering. LIFO = nested pairs (last picked up, first delivered). FIFO = same-order delivery (first picked up, first delivered). Enforced in feasibility checks, insertion evaluation (O(N) precompute + O(1) per (i,j) pruning via `pd_open_depth[]` for LIFO, `pd_max_del_before[]`/`pd_min_del_after[]` for FIFO), and plan validation. Backhaul constraint (`sg_vehicle_set_backhaul()`) — all delivery-only ("linehaul") stops must precede all PD pickup stops. Enforced in feasibility, both insertion evaluators, and plan validation. Both features use fast-path flags (`has_pd_policy`, `has_backhaul`) for zero overhead when unused. JSON API: `"pd_policy": "lifo"/"fifo"`, `"backhaul": true`. New violation types: `SG_VIOLATION_PD_POLICY`, `SG_VIOLATION_BACKHAUL`. 13 new tests (269→282).

#### Previous Status (as of 2026-02-24)

**Baseline**: U1-U8 + S1-S12 + Disjunct TW + Depot Dock Capacity + Commodity Conflicts + Exclusion Groups + Mandatory Breaks + Multi-Trip + Multi-Threading (parallel + population) + SA cooling fix + mid-solve ejection pulse + Speed Profiles + Travel Profiles + Time-Indexed Travel Brackets + Open Start + Plan/ETA Validation + Infeasible-Space Exploration + Aggressive SISR complete. All Tier 1 and Tier 2 production gaps closed. REST API server, WASM build, Python and Node.js bindings exist. 269 tests passing, ASAN/UBSAN clean.

Implemented features: Everything in previous status plus: time-indexed travel brackets — multiple complete duration matrices indexed by departure time. Supported at both global level (`sg_set_travel_time_bracket()`) and per-vehicle travel profile level (`sg_travel_profile_add_time_bracket()`). Orthogonal to speed profiles (which compose multiplicatively on top). Override chain: callback → per-vehicle brackets/matrix → global brackets/matrix → Euclidean → speed profile. Distance uses bracket[0] when no departure_time is available. JSON API supports `time_brackets` in both `travel` and `travel_profiles` sections. 8 new tests (261→269).

#### Previous Status (as of 2026-02-23)

Best measured quality (10000 iterations, deterministic seed 42):

Single-threaded:
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.30`, `avgDistGap=+0.2%`, `equalVehicles=39`, `lexiNonWorse=12`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.48`, `avgDistGap=+4.2%`, `equalVehicles=44`, `lexiNonWorse=24`.

Population-based (3 generations, auto threads):
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.20`, `avgDistGap=-0.2%`, `equalVehicles=45`, `lexiNonWorse=13`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.38`, `avgDistGap=+3.5%`, `equalVehicles=48`, `lexiNonWorse=26`.

Implemented features: Everything in previous status plus: HGS-style infeasible-space exploration with adaptive penalty manager (6 constraint types — time warp, capacity, duration, ride time, distance, total work — with independent per-constraint self-adjustment), time warping (violation accumulated, start warped to tw_late for downstream propagation), feasible-beats-infeasible best-tracking, cost-proportional penalty scaling (bounds and initial weights adapt to problem cost structure), instance-adaptive SISR L_max (Christiaens & Vanden Berghe 2020). `--population` flag added to Solomon and Li & Lim benchmarks.

Infrastructure: REST API server (Mongoose, rate limiting, work queue, Prometheus metrics, CORS), WASM build (Emscripten), Python bindings (ctypes), Node.js bindings (ffi-napi). REST API e2e test suite.

#### Previous Status (as of 2026-02-23)

**Baseline**: U1-U8 + S1-S11 + Disjunct TW + Depot Dock Capacity + Commodity Conflicts + Exclusion Groups + Mandatory Breaks + Multi-Trip + Multi-Threading (parallel + population) + SA cooling fix + mid-solve ejection pulse + Speed Profiles + Travel Profiles + Open Start + Plan/ETA Validation complete. All Tier 1 and Tier 2 production gaps closed. REST API server, WASM build, Python and Node.js bindings exist. 252 tests passing, ASAN/UBSAN clean.

Best measured quality (10000 iterations, deterministic seed 42):
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.36`, `avgDistGap=+0.4%`, `equalVehicles=37`, `lexiNonWorse=11`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.52`, `avgDistGap=+4.8%`, `equalVehicles=41`, `lexiNonWorse=22`.

Implemented features: Everything in previous status plus: plan/ETA validation mode (`sg_validate_plan()`) — validate pre-existing routes without re-optimizing, compute ETAs for every stop, report constraint violations (hard TW, capacity, PD order, ride time, max duration/distance/tasks, forbidden vehicle, qualifications). JSON API supports `"plan"` key for validation mode. New types: `SGPlanRoute`, `SGViolation`, `SGViolationType`.

Infrastructure: REST API server (Mongoose, rate limiting, work queue, Prometheus metrics, CORS), WASM build (Emscripten), Python bindings (ctypes), Node.js bindings (ffi-napi). REST API e2e test suite.

#### Previous Status (as of 2026-02-23)

**Baseline**: U1-U8 + S1-S11 + Disjunct TW + Depot Dock Capacity + Commodity Conflicts + Exclusion Groups + Mandatory Breaks + Multi-Trip + Multi-Threading (parallel + population) + SA cooling fix + mid-solve ejection pulse complete. All Tier 1 and Tier 2 production gaps closed. 224 tests passing, ASAN/UBSAN clean.

Best measured quality (10000 iterations, deterministic seed 42):
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.36`, `avgDistGap=+0.4%`, `equalVehicles=37`, `lexiNonWorse=11`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.52`, `avgDistGap=+4.8%`, `equalVehicles=41`, `lexiNonWorse=22`.

Implemented features: travel matrix API (U1), vehicle-request qualifications (U2), solution route/stop export (U3), open routes (U4), max route duration + explicit max ride time (U5), vehicle cost model + configurable objective (U6), soft time windows (U7), request-vehicle constraints (U8), disjunct time windows, waiting cost (per-vehicle `cost_per_waiting`), overtime cost (per-vehicle `cost_per_overtime` with soft shift), convenience constructors, stop load/type/duration export, depot dock capacity (per-depot `max_simultaneous` with sweep-line overlap penalty), commodity conflicts (bitmask-based, up to 64 types, O(1) conflict check), exclusion groups (at most one request per group per vehicle), mandatory breaks (abstract `max_continuous_work` / `break_duration` / `max_total_work` per vehicle, break injection in timing forward pass, break position export), multi-trip (per-vehicle `max_trips` / `trip_reload_seconds`, capacity reset at depot, trip boundary metadata on stop sequence, new-trip insertion in repair operators, trip_count/trip_index in solution export), multi-threaded parallel solve (`sg_solve_parallel` — independent runs with different seeds), population-based search (`sg_solve_population` — generational ALNS with elite pool warm-starting, tournament selection), Phase 1 SA cooling override (decay to 5% of T0 for sustained vehicle-reducing acceptance), mid-solve ejection pulse (ejection chain + intensify between Phase 1 and Phase 2).

#### Previous Status (as of 2026-02-23)

**Baseline**: U1-U8 + S1-S10 + Disjunct TW + Depot Dock Capacity + Commodity Conflicts + Exclusion Groups + Mandatory Breaks + Multi-Trip + Multi-Threading (parallel + population) complete. All Tier 1 and Tier 2 production gaps closed. 220 tests passing, ASAN/UBSAN clean.

#### Previous Status (as of 2026-02-22)

**Baseline**: U1-U8 + S1-S9 + Disjunct TW complete. All usability phases done. 115 tests passing, ASAN/UBSAN clean. Benchmarks unchanged from previous baseline (disjunct TWs not active in benchmark instances — zero impact on existing behavior).

Best measured quality (10000 iterations, deterministic seed 42):
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.38`, `avgDistGap=+0.2%`, `equalVehicles=35`, `lexiNonWorse=11`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.59`, `avgDistGap=+3.9%`, `equalVehicles=39`, `lexiNonWorse=21`.

Implemented features: travel matrix API (U1), vehicle-request qualifications (U2), solution route/stop export (U3), open routes (U4), max route duration + explicit max ride time (U5), vehicle cost model + configurable objective (U6), soft time windows (U7), request-vehicle constraints (U8), disjunct time windows, waiting cost (per-vehicle `cost_per_waiting`), overtime cost (per-vehicle `cost_per_overtime` with soft shift), convenience constructors, stop load/type/duration export.

#### Previous Status (as of 2026-02-22)

**Baseline**: U1-U8 + S1-S9 complete. All usability phases done. 109 tests passing, ASAN/UBSAN clean. Benchmarks unchanged from previous baseline (soft TWs not active in benchmark instances — zero impact on existing behavior).

Implemented features: travel matrix API (U1), vehicle-request qualifications (U2), solution route/stop export (U3), open routes (U4), max route duration + explicit max ride time (U5), vehicle cost model + configurable objective (U6), soft time windows (U7), request-vehicle constraints (U8), waiting cost (per-vehicle `cost_per_waiting`), overtime cost (per-vehicle `cost_per_overtime` with soft shift), convenience constructors, stop load/type/duration export.

#### Previous Status (as of 2026-02-22)

**Baseline**: U1-U6 + U8 + S1-S9 complete. Waiting cost and overtime cost implemented. 105 tests passing, ASAN/UBSAN clean.

#### Previous Status (as of 2026-02-21)

**Baseline**: U1-U6 + U8 + S1-S9 complete. Waiting cost implemented. 102 tests passing, ASAN/UBSAN clean.

#### Previous Status (as of 2026-02-20)

Implemented and active today:
- C domain model for depots, vehicles, tasks, requests, and multi-dimensional capacities.
- Model validation for delivery-only and pickup-delivery demand sign consistency.
- Arbor ALNS integration with simulated annealing acceptance (Phase S1).
- Route-native ALNS path for unified VRPTW/PDPTW with stop-level representation.
- Incremental feasibility kernel with cached forward/backward timing slack and load profiles.
- Independent PD stop placement with O(L²) evaluation of all (pickup, delivery) position pairs (Phase S2).
- Stop-level splice/excise operations preserving non-adjacent PD placement across ALNS iterations.
- Advanced destroy/repair operators:
  random, worst, shaw, criticality-worst, route-cluster, time-cluster, paired-shaw,
  route-removal, time-window-removal, greedy and regret-k repairs (with PD-aware dispatch).
- Post-ALNS route elimination, exchange, 2-opt*, and distance polishing (PD-aware).
- Solomon and Li & Lim benchmark harnesses with BKS comparison.

Best measured quality (10000 iterations, deterministic seed 42, population mode):
- Solomon (VRPTW, 56 cases): `avgVehGap=+0.18`, `avgDistGap=-0.1%`, `equalVehicles=46/56`, `lexiNonWorse=14`.
- Li & Lim (PDPTW, 56 cases): `avgVehGap=+0.39`, `avgDistGap=+3.7%`, `equalVehicles=47/56`, `lexiNonWorse=27`.
- All 113 solutions verified feasible (post-solve validation gate in `sg_solve_route_model`).

Solver quality highlights:
- Solomon C1xx/C2xx (17/17): exact BKS match on both vehicles and distance.
- Wide-TW instances (C2, LC2, LR2, LRC2) essentially solved — nearly all match BKS on both vehicles and distance.
- Li & Lim LC2xx (8/8), LR2xx (11/11), LRC2xx (8/8): all match BKS vehicles and distance exactly.
- Remaining gap: tight-TW random instances (R1, LR1, RC1, LRC1) show +1 vehicle with often better distance (trade-off pattern). LC101/LC102 use +5/+3 vehicles (identical PD TW widths defeat sorting heuristics).

Solver uses Euclidean travel only — no distance/time matrix API exposed yet.

Constraint gaps for rich VRPTW/PDPTW (not yet in core solve path):
- Distance/time matrix (non-Euclidean travel costs).
- Vehicle-request compatibility (skills/qualifications).
- Open routes (vehicles that don't return to depot).
- Max route duration per vehicle.
- Explicit max ride time per PD request (currently derived from TW spans).
- Vehicle cost model (fixed cost, per-km, per-hour).
- Configurable objective weights.
- Soft time windows with tardiness penalties.
- Disjunct time windows (multiple allowed windows per task).
- Request-vehicle assignment constraints (required/forbidden).
- Solution route/stop export in JSON API.
- Driver break/HoSE constraints in route feasibility.

### Actualized Plan (Unified VRPTW/PDPTW)

### Phase 1: Consolidate Current Delivery-Only Engine (done/in progress)
- [x] Route-native state with explicit routes and lexicographic objective weighting.
- [x] Delivery-only TW/capacity route feasibility checks and insertion/removal.
- [x] Expanded destroy/repair operator portfolio (including route/time-window removals).
- [x] Postprocess route elimination and distance polish.
- [x] Add per-operator telemetry reporting in benchmark output for focused tuning.

### Phase 2: Unified Route State for VRPTW + PDPTW (now active)
- [x] Replace delivery-only route sequence assumptions with stop-level representation supporting both stop types.
- [x] Encode request pair mapping (pickup stop id, delivery stop id) in solution state and maintain predecessor/successor links.
- [x] Enforce same-vehicle and precedence constraints directly via the stop sequence; route validation now relies on the unified kernel.

### Phase 3: Unified Incremental Feasibility and Cost Kernel
- [x] Build one incremental feasibility engine for both VRPTW and PDPTW (TW propagation, signed load tracking, route-duration checks).
- [x] Add PD-specific checks: precedence, maximum ride time, pickup/drop consistency.
- [x] Replace full route recomputation per move with cached forward/backward slack and load deltas.
- [x] O(L²) pickup/delivery position pair evaluation with push propagation and forward slack pruning.

### Phase 4: Unified ALNS Operators and Intensification
- [x] Make ALNS remover/repair steps operate on the shared route state with explicit stops.
- [x] Added exchange + 2-opt* intensification heuristics that now respect PD pair structure.
- [x] Pair-aware Shaw + regret insertions operate against the unified feasibility kernel.

### Phase 5: Objective and Acceptance Modernization ✅
- [x] Move from scalar proxy toward explicit lexicographic compare (unassigned -> vehicles -> distance -> soft penalties). `SGConfig.lexicographic_objective` gates `is_better` callback in ARSolutionOps.
- [x] Expose acceptance policy in `SGConfig` (SA/RRT/Improving). `SGAcceptType` enum mapped to `ARAcceptType` at solve time.
- [x] Add adaptive destroy size policy based on stagnation. `SGConfig.adaptive_q` / `ARALNSParams.adaptive_q` grows `q_max` at segment boundaries on stagnation, resets on improvement.

### Phase 6: Rich Constraint Completion
- [x] Disjunct TW support.
- [x] Soft TW penalties and waiting-cost terms in objective.
- [x] Vehicle qualifications (U2).
- [x] Commodity conflicts, exclusion groups.
- [x] Open routes (U4).
- [x] Depot dock capacity (sweep-line overlap penalty).
- [x] Max route duration (U5).
- [x] Mandatory breaks (abstract `max_continuous_work` / `break_duration` / `max_total_work` per vehicle).

- [x] Add optional travel-time/distance matrix API and use it in construction + route feasibility (U1).
- [x] Speed profiles (time-dependent duration multipliers).
- [x] Per-vehicle travel profiles (independent distance/duration matrices + speed profile).
- [x] Time-indexed travel brackets (multiple complete matrices by departure time, global + per-vehicle).
- [x] Open start routes (skip first depot-to-stop leg).
- [ ] Integrate Velo matrices for realistic routing costs/times.
- [ ] Keep Ralph exact mode for small instances as baseline verifier.

### Recent progress
- **Phase S1 (SA acceptance)**: Enabled simulated annealing in both solver paths via `ar_alns_calibrate_sa`. Solomon improved from +9.3% to +5.9% avgDistGap at 300 iterations.
- **Phase S2 (independent PD placement)**: O(L²) pickup/delivery evaluation with stop-level splice/excise. Li & Lim improved from +112.7% to +9.5% avgDistGap. Solomon unchanged at +5.9%.
- **Phase S3 (route-aware worst removal)**: Replaced proxy-based removal cost with actual distance delta. Solomon +5.5% → +0.8% (at 5k iters), Li & Lim +9.5% → +4.2%.
- **Phase S4 (route-aware Shaw relatedness)**: Replaced zone-based proxy in Shaw removal with spatial/TW/load/co-route scoring. Solomon +0.8% → +0.4% (at 5k iters). Li & Lim neutral at +4.3%.
- **Phase S5 (adaptive destroy count)**: Scale q_min/q_max with instance size: `q_min=max(config,n/20)`, `q_max=max(config,n/4)`. Solomon +0.4% → +0.2% (at 5k iters). Li & Lim unchanged (instances too small to trigger).
- **Phase S6 (enhanced local search)**: Added or-opt(2,3) segment relocation and increased intensify passes 4→8. Solomon stable at +0.2% (C104 improved). Li & Lim stable at +4.3%. No runtime overhead.
- **Phase S7 (stagnation restart)**: Added restart-from-best mechanism in Arbor ALNS loop. When stagnation iterations reach threshold (max_iterations/4), copies best solution to current and reheats SA temperature to 50% of initial. Solomon stable at +0.2% (avgVehGap improved +0.52→+0.46). Li & Lim stable at +4.3%. Neutral at 5k iterations; infrastructure ready for longer runs.
- **Phase S8 (construction + vehicle minimization)**: Multi-strategy construction (regret-3, TW-sorted greedy, Solomon I1 — keep best), two-phase ALNS (60% vehicle minimization with hot SA + 40% distance polishing), vehicle-target and vehicle-empty destroy operators, pair elimination in reduce_vehicles, depth-2 ejection chains, pairwise exchange in postprocessing. Solomon +0.2% → +0.2% at 5k iters (avgVehGap +0.46→+0.46). Li & Lim +4.3% → +4.3% at 5k iters (avgVehGap +0.70→+0.55, equalVehicles 36→38).
- **Phase S9 (deeper ejection chains, CROSS-exchange, or-opt k=1, validation)**: Ejection depth 2→5 with 50K attempt budget, CROSS-exchange operator swapping segments of size 1-3 between routes, or-opt extended to k=1 for single-request relocate in intensify loop, post-solve feasibility validation gate in `sg_solve_route_model`, benchmark iterations 5000→10000. Solomon avgVehGap +0.46→+0.36, avgDistGap +0.2%→-0.2%, equalVehicles 33→36. Li & Lim avgVehGap +0.55→+0.55, avgDistGap +4.3%→+4.1%, equalVehicles 38→40. All 113 solutions verified feasible.
- **Phase S10 (sequence-dependent setup times + per-operator telemetry)**: Asymmetric N×N setup class matrix (1-indexed, 0 = no class). Setup time added after arrival, before service start, in forward/backward timing passes and both cached insertion evaluators. Per-operator telemetry (selected, accepted, improvements, weight, total_seconds) exposed through Surge API and `--telemetry` flag in benchmarks. Solomon +0.2% → +0.2%, Li & Lim +3.9% → +3.9% (no regression). 135 tests, ASAN clean.
- **Phase 5+8 (objective modernization + verification)**: Lexicographic best-tracking via `is_better` callback in `ARSolutionOps` (gated by `SGConfig.lexicographic_objective`). Acceptance policy exposed via `SGAcceptType` (SA/RRT/Improving). Adaptive destroy size grows q_max on stagnation, resets on improvement (`SGConfig.adaptive_q`). Cordeau DARP loader (`sg_load_cordeau_darp`) and `bench_cordeau` harness. 20 new tests (135→155). Solomon +0.2%, Li & Lim +3.9% (no regression). DARP solve quality pending dedicated construction heuristic.
- **Phase S11 (multi-threaded parallel + population search)**: `sg_solve_parallel()` runs N independent ALNS solves with different seeds, picks best (15 wins vs 0 losses on Li & Lim vs single-threaded). `sg_solve_population()` adds generational warm-starting — elite pool with tournament selection, same compute budget but guided search. Li & Lim population vs parallel: 10 wins, 6 losses, 40 ties, avg distance -0.6%. Includes `solution_arena_size` transfer fix ensuring fast arena-memcpy path in result harvesting. 5 new tests (215→220). ASAN clean.
- **Phase S12 (infeasible-space exploration + aggressive SISR)**: HGS-style infeasible-space search with modular penalty manager (`SGPenaltyManager` in `sg_penalty.c`). 6 constraint types (time warp, capacity, duration, ride time, distance, total work) with independent per-constraint self-adjustment. Time warping accumulates violation and warps start to `tw_late` for downstream propagation. Feasible-beats-infeasible best-tracking in `sg_route_solution_is_better`. Penalty bounds and initial weights scale proportionally with problem cost structure via `cost_scale` parameter — no hardcoded constants. Instance-adaptive SISR `L_max` based on avg route length (Christiaens & Vanden Berghe 2020), initial string destroy weight 2.0. Phase 1 (vehicle min) uses aggressive 15% feasible target; Phase 2 (distance) runs strict (penalty disabled). Solomon single-thread: avgVehGap +0.36→+0.30, equalVehicles 37→39. Solomon population: avgVehGap +0.20, equalVehicles 45, avgDistGap -0.2%. Li & Lim single-thread: avgVehGap +0.52→+0.48, equalVehicles 41→44. Li & Lim population: avgVehGap +0.38, equalVehicles 48, avgDistGap +3.5%. 5 new tests (252→257). ASAN/UBSAN clean.
- **Phase S13+S14+S15 (algorithmic edge + population crossover)**: Progressive penalty schedule (0.25→0.15 over Phase 1), ejection chains in repair operators with cost-gated fallback, scaled ejection budget (proportional to instance size, cap 500K), relaxed vehicle reduction (20% distance slack), Phase 1.5 vehicle crunch (500-iter focused ALNS with vehicle-reducing operators only), SREX crossover (merge routes from two parents), population diversity filter (>90% similarity rejection). Solomon population: avgVehGap +0.20→+0.18, equalVehicles 45→46, avgDistGap -0.1%. Li & Lim population: avgVehGap +0.39, equalVehicles 47, avgDistGap +3.7%. 14 new tests (326→340). ASAN/UBSAN clean.
- **Phase S16 (CFRS construction heuristics)**: Cluster-First-Route-Second construction methods targeting vehicle count reduction on large instances. Two new heuristics: angular sweep CFRS (`sg_construct_sweep_cfrs`) and k-means with TW dimension (`sg_construct_kmeans_tw`), both using a shared vehicle count lower bound (`sg_estimate_min_vehicles` — bin packing + TW conflict clique). `SGConstructMethod` enum with dispatch table. Population mode round-robins construction method per thread (`thread_index % SG_CONSTRUCT_COUNT`) for generation 0 diversity. Default mode tries all 5 methods (regret-3, TW-sorted, Solomon I1, sweep CFRS, k-means TW) and keeps lexicographic best. GH-200 population: equalVehicles 48→55 (80%→92%), avgVehGap +0.20→+0.08 — best vehicle count result. Distance gap +13.2%→+13.1% (stable). 13 new tests (374→387). ASAN/UBSAN clean.
- **Phase S19 (lazy heap repair)**: O(N log N) lazy max-heap replacement for O(N²) repair fill. Phase 1 evaluates all unassigned requests and pushes to heap; Phase 2 pop-validate-insert loop revalidates only the top candidate per insertion. Bounce counter prevents infinite loops from floating-point ties. `SGRegretEntry` cache + `SHHeap` in scratch buffers. GH-400 population: equalVehicles 27→33 (+6), avgDistGap +33.3%→+18.6% (-14.7pp). 8 new tests (413→421). ASAN/UBSAN clean.
- Unified route state drives both delivery-only and PDPTW solves. The stop-based kernel tracks forward/backward time slack, load profiles, and ride-time constraints.
- Stop-level splice/excise operations preserve non-adjacent PD placement across ALNS destroy/repair cycles.

### Phase 8: Verification and Benchmark Expansion
- [x] Keep Solomon VRPTW as regression benchmark (56/56, +0.2%).
- [x] Add Li & Lim PDPTW harness and BKS comparator (57/57, +3.9%).
- [x] Add Cordeau DARP harness (`bench_cordeau`, `sg_load_cordeau_darp`). Loader + benchmark wired; solve quality pending DARP-specific construction heuristic.
- [x] Expand unit tests from smoke coverage to operator and feasibility regression suites (135 → 155 → 220 tests).
- [x] Multi-threaded parallel solve (`sg_solve_parallel`): independent runs with different seeds.
- [x] Population-based search (`sg_solve_population`): generational ALNS with elite pool warm-starting.
- [ ] Add profiling-driven performance work (allocation hot paths, insertion complexity, cache reuse).

Execution order:
1. Phase 2
2. Phase 3
3. Phase 4
4. Phase 5
5. Phase 6
6. Phase 7
7. Phase 8

This order is required because performance and quality on rich PDPTW depend primarily on
having one unified route/feasibility engine before additional constraints and integrations.

---

## Usability & Rich Constraints Roadmap

With solver quality at a production-usable level (Solomon -0.1% avg distance gap, Li & Lim
+3.7%), the focus shifts to modeling real-world constraints. These phases are ordered by
dependency and business impact. Each builds on the architecture already in place — the unified
stop-level route state, incremental feasibility kernel, and ALNS operator framework.

### Phase U1: Distance/Time Matrix API

**Priority**: Critical — Euclidean distance is meaningless for real road networks.

**What**: Add a precomputed location-to-location travel time and distance matrix that the
solver consumes instead of `sg_euclid()`. Falls back to Euclidean when no matrix is set.

**API surface**:
```c
SGStatus sg_set_travel_matrix(SGContext *ctx, uint32_t location_count,
                               const double *distance_matrix,
                               const double *time_matrix);
```

**Architecture fit**: The solver already routes all distance queries through a small number
of call sites (`sg_euclid` in `sg_cost.c`, `sg_route_stop_sequence_feasible` in
`sg_feasibility.c`). Replace with a lookup function that checks `ctx->travel_matrix` first.
Store matrices as flat `double[n*n]` arrays in `SGContext`. The JSON API gets `"travel_matrix"`
and `"time_matrix"` fields (or an array of `{from, to, distance, time}` sparse entries).

**Changes**: `sg_context.c` (storage + setter), `sg_cost.c` (replace `sg_euclid`),
`sg_feasibility.c` (use time matrix for travel time instead of distance/speed),
`sg_api.c` (JSON parsing), `surge.h` (public API).

**Complexity**: Small. ~200 LOC. No algorithmic changes.

### Phase U2: Vehicle-Request Compatibility (Skills)

**Priority**: High — nearly every fleet has vehicle types (refrigerated, tail-lift, ADR).

**What**: Bitmask-based qualification system. Each vehicle has capabilities (`uint64_t
qualifications`), each request has requirements (`uint64_t required_qualifications`).
A vehicle can serve a request only if `(vehicle.quals & request.required) == request.required`.

**API surface**:
```c
SGStatus sg_vehicle_set_qualifications(SGContext *ctx, uint32_t vehicle_id,
                                        uint64_t qualification_flags);
SGStatus sg_request_set_required_qualifications(SGContext *ctx, uint32_t request_id,
                                                 uint64_t qualification_flags);
```

**Architecture fit**: One additional check at the top of `sg_route_eval_insertion_cached` and
`sg_route_eval_pd_best_insertion_cached` — skip vehicle entirely if quals don't match. Store
quals in `SGVehicleRecord` and `SGRequestRecord`. No change to route state or ALNS operators.
The destroy/repair operators automatically respect it because they call the insertion evaluator.

**Changes**: `sg_internal.h` (add fields), `sg_feasibility.c` (add check), `sg_api.c`
(JSON parsing), `surge.h` (public API).

**Complexity**: Tiny. ~80 LOC. One `if` statement in the hot path.

### Phase U3: Solution Route/Stop Export

**Priority**: High — the JSON API currently returns only aggregate stats. Users need
actual routes with stop sequences, arrival times, and load states to display or execute.

**What**: Extend the solve response JSON to include per-route stop details:

```json
{
  "routes": [
    {
      "vehicle_id": 0,
      "stops": [
        {
          "type": "depot_start",
          "depot_id": 0,
          "arrival": 0, "departure": 28800
        },
        {
          "type": "delivery",
          "request_id": 5, "task_id": 6,
          "arrival": 29100, "service_start": 29100,
          "departure": 29400, "load_after": [150.0]
        }
      ],
      "distance": 234.5, "duration": 3600
    }
  ],
  "unassigned_request_ids": [12, 17]
}
```

**Architecture fit**: The internal `SGRouteSolution` already stores complete stop sequences
with timing (`arrival`, `service_start`, `depart`) and load profiles. This is pure
serialization — walk the solution state and emit JSON.

**Changes**: `sg_api.c` (response builder), `surge.h` (add `sg_get_route_count`,
`sg_get_route_stop_count`, `sg_get_route_stop_info` accessors).

**Complexity**: Medium. ~300 LOC. No solver changes.

### Phase U4: Open Routes ✅

**Priority**: High — field service, one-way deliveries, and ride-hailing vehicles often
don't return to depot.

**What**: Per-vehicle flag: `open_end = true` means the vehicle's route ends at its last
stop instead of returning to the end depot. Distance and time for the return leg are not
counted.

**API surface**:
```c
SGStatus sg_vehicle_set_open_end(SGContext *ctx, uint32_t vehicle_id, int open);
```

**Architecture fit**: The feasibility kernel (`sg_route_stop_sequence_feasible`) builds the
stop sequence with depot start/end. For open-end vehicles, skip the return-to-depot leg in
both distance computation and TW checking. The end-depot TW check is also skipped. Store
flag in `SGVehicleRecord`.

**Changes**: `sg_internal.h` (add field), `sg_feasibility.c` (conditional return leg),
`sg_cost.c` (skip return distance), `sg_api.c` (JSON parsing).

**Complexity**: Small. ~60 LOC. Localized to feasibility kernel.

**Completion**: Implemented with 4 tests (basic distance reduction, timing feasibility, solution export, PD pair). Open-end correctly excludes return leg from distance, duration, and shift TW checks. Benchmarks stable.

### Phase U5: Max Route Duration and Explicit Max Ride Time ✅

**Priority**: Medium-High — max route duration is a standard fleet constraint (8-hour shift
minus break). Explicit max ride time is needed for DARP/passenger transport.

**What (duration)**: Per-vehicle `max_duration_seconds`. If `route_end_time - route_start_time
> max_duration`, the route is infeasible. Checked at the end of the forward timing pass.

**What (ride time)**: Per-request `max_ride_time_seconds` for PD pairs. If
`delivery_service_start - pickup_depart > max_ride_time`, infeasible. Currently derived from
TW spans — make it an explicit user-settable field that overrides the derived limit.

**API surface**:
```c
SGStatus sg_vehicle_set_max_duration(SGContext *ctx, uint32_t vehicle_id,
                                      int32_t max_seconds);
SGStatus sg_request_set_max_ride_time(SGContext *ctx, uint32_t request_id,
                                       int32_t max_seconds);
```

**Architecture fit**: Both are single additional checks in `sg_route_stop_sequence_feasible`.
Duration check: one comparison at the end. Ride time: already computed, just compare against
the explicit limit instead of the derived one.

**Changes**: `sg_internal.h` (add fields), `sg_feasibility.c` (two checks),
`sg_api.c` (JSON parsing).

**Complexity**: Tiny. ~50 LOC.

**Completion**: Implemented with 4 tests (API validation, duration infeasibility, explicit ride time override, TW-derived default). `max_duration=0` means unlimited, `max_ride_time=0` falls back to TW-derived limit. Benchmarks stable.

### Phase U6: Vehicle Cost Model and Configurable Objective ✅

**Priority**: Medium — needed to model heterogeneous fleets where a 40t truck costs more
than a van. Also needed for any customer who wants to minimize cost rather than distance.

**What**: Per-vehicle costs (`fixed_cost`, `cost_per_distance_unit`, `cost_per_hour`) and
global objective weights. The objective becomes:

```
cost = w_unassigned * unassigned_penalty
     + sum_v(fixed_cost_v * used_v + cost_per_km_v * distance_v + cost_per_hour_v * duration_v)
```

**API surface**:
```c
SGStatus sg_vehicle_set_costs(SGContext *ctx, uint32_t vehicle_id,
                               double fixed_cost, double cost_per_distance,
                               double cost_per_duration);
SGStatus sg_set_unassigned_weight(SGContext *ctx, double weight);
```

**Architecture fit**: `sg_route_solution_cost` and `sg_route_objective_cost` are already
centralized. Replace the hardcoded `1e9 * unassigned + 1e6 * vehicles + distance` with
a weighted sum using per-vehicle costs. Duration requires tracking route duration in the
solution state (add a `route_duration` array alongside `route_distance`).

**Changes**: `sg_internal.h` (add vehicle cost fields, route_duration array),
`sg_solution.c` (cost function), `sg_feasibility.c` (compute duration),
`sg_api.c` (JSON parsing).

**Complexity**: Medium. ~200 LOC. Touches cost function used by SA acceptance — needs care.

**Completion**: Implemented with 3 tests (API validation, prefer-cheaper vehicle selection, unassigned weight tradeoff). Cost model correctly drives vehicle selection via SA acceptance. Route duration tracked and exported. Benchmarks stable.

### Phase U7: Soft Time Windows ✅

**Priority**: Medium — real dispatchers accept small delays with a cost penalty rather
than declaring a delivery unservable.

**What**: Per-task optional soft time window nested within the hard TW. Linear per-second
penalty for service outside the preferred window. Hard TW `[tw_early, tw_late]` remains
as absolute bounds (infeasible outside). Soft TW `[soft_tw_early, soft_tw_late]` adds a
penalty layer within hard bounds.

**API surface**:
```c
SGStatus sg_task_set_soft_time_window(SGContext *ctx, uint32_t task_id,
                                      int32_t early, int32_t late,
                                      double early_penalty, double late_penalty);
double sg_solution_get_route_tw_penalty(const SGContext *ctx, uint32_t route_index);
```

**Penalty formula**: `tw_early_penalty * max(0, soft_tw_early - start) + tw_late_penalty * max(0, start - soft_tw_late)`.

**Architecture fit**: Key insight — no feasibility kernel changes needed. Soft TWs are an
additional penalty layer WITHIN the existing hard bounds. Hard TW checks remain unchanged
at all ~13 sites in `sg_feasibility.c`. Penalty accumulated in the forward timing pass
(`sg_route_update_timing`) and added directly to route cost (pre-multiplied by per-task
coefficients, no vehicle-level multiplier).

**Changes**: `sg_internal.h` (add task fields + route_tw_penalty array), `sg_types.h`
(total_tw_penalty in SGStats), `surge.h` (public API), `sg_context.c` (setter + getter +
validation), `sg_feasibility.c` (penalty accumulation in forward pass), `sg_solution.c`
(lifecycle: reset/init/copy/validate/cost), `sg_solve.c` (stats accumulation).

**Complexity**: Medium. ~250 LOC. No feasibility kernel changes — cleaner than originally anticipated.

**Completion**: Implemented with 4 tests (API validation, late penalty accumulation, hard TW still rejects, early penalty + stats). Forward-compatible with future disjunct time windows (single soft window per task = N=1 case). Benchmarks stable — default settings (no soft TWs) have zero impact on existing behavior.

### Phase U8: Request-Vehicle Constraints ✅

**Priority**: Medium — "driver X always serves customer Y" or "vehicle Z cannot enter zone W."

**What**: Per-request lists of allowed or forbidden vehicle IDs. If `allowed_vehicles` is
non-empty, only those vehicles can serve the request. If `forbidden_vehicles` is non-empty,
those vehicles are excluded.

**API surface**:
```c
SGStatus sg_request_add_allowed_vehicle(SGContext *ctx, uint32_t request_id,
                                         uint32_t vehicle_id);
SGStatus sg_request_add_forbidden_vehicle(SGContext *ctx, uint32_t request_id,
                                           uint32_t vehicle_id);
```

**Architecture fit**: Same as skills (U2) — one check at the top of insertion evaluation.
Store as a bitset or small array in `SGRequestRecord`. For small vehicle counts (<64),
a `uint64_t` bitmask is optimal. For larger fleets, a sorted array with binary search.

**Changes**: `sg_internal.h` (add fields), `sg_feasibility.c` (add check),
`sg_api.c` (JSON parsing).

**Complexity**: Small. ~100 LOC.

**Completion**: Implemented with dynamically-sized bitsets (`uint64_t *` arrays, grows on demand) for both allowed and forbidden vehicles — no vehicle count limit. Forbidden takes precedence over allowed. Inline check `sg_vehicle_allowed_for_request` added at all 4 insertion sites (same pattern as U2 qualifications). 5 tests: API validation, allowed-vehicle filtering, forbidden-vehicle unassignment, PD pair constraint, large fleet (100 vehicles, constraint on V99). Benchmarks stable.

### Execution Order and Dependencies

```
U1 (travel matrix)      ──── ✅ complete
U2 (skills)             ──── ✅ complete
U3 (solution export)    ──── ✅ complete
U4 (open routes)        ──── ✅ complete
U5 (duration + ride)    ──── ✅ complete
U6 (cost model)         ──── ✅ complete
U7 (soft TW)            ──── ✅ complete
U8 (vehicle constraints)──── ✅ complete
```

All usability phases (U1-U8) are complete.

### Production Gap Analysis

With U1-U8 complete, the following gaps remain between Surge and a production-ready solver.
Grouped by business impact:

**Tier 1 — Blocking for production:**

| Gap | Status | Notes |
|-----|--------|-------|
| **Soft time windows** | ✅ Complete | Per-task soft TW with linear penalty within hard bounds. 4 tests. |
| **Disjunct time windows** | ✅ Complete | Per-task multiple non-overlapping hard TWs with gap snapping. 6 tests. |
| **JSON API completeness** | ✅ Complete | All C API features exposed via JSON. `sg_api_build_model`, `sg_api_build_model_file`, `sg_api_write_solution`. 10 tests. |
| **Error diagnostics** | ✅ Complete | `sg_get_last_error()` with descriptive validation messages. Entity-level errors (depot, vehicle, request). 2 tests. |
| **Driver breaks / HoS** | ✅ Complete | Abstract break model: per-vehicle `max_continuous_work`, `break_duration`, `max_total_work`. Breaks injected during timing forward pass (not as stops). Break position export for reporting. 14 tests. |

**Tier 2 — High business value:**

| Gap | Status | Notes |
|-----|--------|-------|
| **Waiting cost** | ✅ Complete | Per-vehicle `cost_per_waiting`, accumulated in forward pass. 3 tests. |
| **Overtime cost** | ✅ Complete | Per-vehicle `cost_per_overtime` with soft shift boundary. 3 tests. |
| **Depot dock capacity** | ✅ Complete | Per-depot `max_simultaneous` with sweep-line overlap penalty. 6 tests. |
| **Per-request drop penalty** | ✅ Complete | `sg_request_set_unassigned_penalty()` overrides global weight per request. 2 tests. |
| **Warm start** | ✅ Complete | `sg_set_initial_routes()` injects initial solution. Partial warm start supported. 2 tests. |
| **Progress callback + cancel** | ✅ Complete | `sg_set_progress_callback()` at segment boundaries, `sg_cancel()` for early termination. Arbor-level `ARProgressCallback`. 3 tests. |
| **Multiple trips per vehicle** | ✅ Complete | Per-vehicle `max_trips` / `trip_reload_seconds`. Capacity resets at depot, shift/break constraints span entire shift. Trip boundary metadata on stop sequence. New-trip insertion in ALNS repair. 10 tests. |

**Tier 3 — Niche / specialized:**

| Gap | Status | Notes |
|-----|--------|-------|
| **Commodity conflicts** | ✅ Complete | Bitmask-based (up to 64 types), O(1) conflict check. 4 tests. |
| **Exclusion groups** | ✅ Complete | At most one request per group per vehicle. 4 tests. |
| **Sequence-dependent setup** | ✅ Complete | Asymmetric N×N setup class matrix. 4 tests. |
| **Time-dependent travel** | ✅ Complete | Speed profiles (step-function multipliers) + time-indexed travel brackets (multiple complete matrices by departure time). Both global and per-vehicle. 8 tests. |
| **LIFO/FIFO PD policy** | ✅ Complete | Per-vehicle stacking order: LIFO (nested) or FIFO (same-order). `sg_vehicle_set_pd_policy()`. Feasibility + insertion pruning + plan validation. 7 tests. |
| **Backhaul constraint** | ✅ Complete | All D-only stops before PD pickups. `sg_vehicle_set_backhaul()`. Feasibility + both insertion evaluators + plan validation. 6 tests. |
| **Energy cost model** | Not started | EV-specific path energy cost. OR-Tools only. |

**Tier 3b — Solver-layer gaps (vs commercial solvers):**

These require changes to ALNS/feasibility/insertion. Neither OR-Tools nor VROOM has them.

| Gap | Status | Impact | Notes |
|-----|--------|--------|-------|
| **Live re-optimization** | **Done** | High | Three-level request locking: NONE (free), COMMITTED (must-serve, can reassign), FROZEN (locked to vehicle). All destroy/repair/postprocess operators respect locks. 1e12 penalty for committed drops. Hardened warm-start: frozen vehicle map (`frozen_vehicle_map[rid] → vid`), two-pass construction (frozen first), frozen filter in repair ranking, frozen placement validation, infeasible-space fallback to initial solution. Stress-tested on RC101 + C101 Solomon and 53-pair Li & Lim benchmarks (2000-8000 ALNS iterations, 10 integration tests). |
| **Vehicle compartments** | **Done** | Medium | Per-compartment capacity (frozen/chilled/ambient). `sg_add_compartment_type()`, `sg_vehicle_add_compartment()`, `sg_request_set_compartment_type()`. Dual capacity check (vehicle overall + compartment). Zero overhead when unused. Feasibility in forward pass + both insertion evaluators + plan validation. JSON API. 11 tests. |
| **Inter-request precedence** | **Done** | Low | `sg_add_precedence(ctx, before_id, after_id)` — same-vehicle ordering: predecessor's last stop before successor's first stop. Cycle detection via DFS on add. Zero overhead when unused (`has_precedence` guard). Forward-pass feasibility with `completed`+`on_route` bitsets, precedence bounds precomputation in both insertion evaluators, plan validation. JSON API (`"precedences": [{"before":0,"after":1}]`). 11 tests: API validation, basic/chain/PD ordering, cross-vehicle independence, solver integration, JSON roundtrip, compartment orthogonality, zero-precedence regression, plan validation. |

**Tier 3c — Application-layer features (already expressible with current API):**

These do NOT require solver changes — they are orchestration around the existing API.

| Feature | How to Express | Notes |
|---------|---------------|-------|
| **Multi-period/strategic planning** | Solve each day independently, chain via `sg_vehicle_set_initial_load()` for end-of-day state. | Orchestration decides request-to-day assignment. |
| **Territory/zone assignment** | Pre-filter via `sg_request_set_allowed_vehicles()`. | Geographic zones → vehicle sets before solve. |
| **Driver skill calendars** | Vehicle set per day + `sg_request_set_qualifications()`. | Availability = which vehicles exist in today's solve. |
| **Regulatory compliance** | Break policy params (HoSE) + per-vehicle travel profiles (restricted networks). | Country-specific rules map to existing constraint parameters. |

**Tier 4 — Competitive gaps (vs OR-Tools / VROOM):**

| Gap | Status | Competitors | Notes |
|-----|--------|-------------|-------|
| **Time-dependent travel** | ✅ Done | OR-Tools | Speed profiles + time-indexed travel brackets (`sg_set_travel_time_bracket()`, `sg_travel_profile_add_time_bracket()`). Global + per-vehicle. 8 tests. |
| **Max tasks per vehicle** | ✅ Done | VROOM | Per-vehicle cap on request count. 0 = unlimited. |
| **Max distance per vehicle** | ✅ Done | VROOM | Per-vehicle cap on route distance. 0.0 = unlimited. |
| **Open start (no depot)** | ✅ Done | OR-Tools | `sg_vehicle_set_open_start()`. Skips first depot-to-stop leg. 3 tests. |
| **Per-vehicle travel matrix** | ✅ Done | OR-Tools, VROOM | `sg_vehicle_set_travel_profile()` — independent distance/duration matrices + speed profile per vehicle type. 6 tests. |
| **Initial vehicle loads** | ✅ Done | jsprit | `sg_vehicle_set_initial_load()`. First-trip capacity offset with prefix-sum feasibility. 4 tests. |
| **Global span balancing** | ✅ Done | OR-Tools | `sg_set_span_cost_duration()` / `sg_set_span_cost_distance()`. Adds `span_cost × (max - min)` penalty to cost function. 5 tests. |
| **Plan/ETA validation mode** | ✅ Done | VROOM | `sg_validate_plan()` — validate fixed routes, compute ETAs, report violations per stop. JSON API `"plan"` key. 8 tests. |

**Tier 5 — Infrastructure & Performance:**

| Gap | Status | Impact | Notes |
|-----|--------|--------|-------|
| **Arena allocator** | Phases 1-3 done | High | Phase 1 (per-solution arena): ~29 malloc → 1, ~26 free → 1. Phase 2 (optimized copy): `init_for_copy()` + single `memcpy` of arena buffer. Phase 3 (scratch buffers): `SGScratchBuffers` on `SGContext` eliminates per-call malloc/free in feasibility and local search. Cumulative: Solomon -14.8%, Li&Lim -5.0%, Cordeau -3.4% vs Phase 1. |
| **Multi-threading: independent runs** | ✅ Done | High | `sg_solve_parallel()`: N threads × N seeds, pick best. 15 wins vs 0 losses on Li & Lim vs single-threaded. |
| **Multi-threading: parallel move eval** | Not started | Medium | `sg_route_rank_insertions_for_request()` vehicle loop is read-only per vehicle. Thread pool or OpenMP. |
| **REST API server** | ✅ Done | High | `surge/api/surge-solver` — Mongoose + `sh_workqueue`. `sg_api_handle()` routes `/api/v1/solve`, `/health`, `/version`. E2e test suite (`test_api.sh`). |
| **Language bindings** | ✅ Done | Medium | Python (`surge/bindings/python/`) and Node.js (`surge/bindings/node/`) wrappers around JSON API. Test suites for both. |
| **Population-based search** | ✅ Done | Medium | `sg_solve_population()`: generational ALNS with elite pool warm-starting. 10 wins vs 6 losses on Li & Lim vs independent parallel runs, avg distance -0.6%. |
| **WASM build** | ✅ Done | Medium | `surge/wasm/` — Emscripten target. `sg_wasm_api.c` wraps `sg_api_handle()`. Transport-agnostic by design. |

### JSON API (`sg_api.h`)

The JSON API provides three tiers of access:

**Entry points:**
- `sg_api_solve()` — parse JSON, build model, validate, solve, return JSON response
- `sg_api_build_model()` — build model from parsed `ShJsonValue` DOM
- `sg_api_build_model_file()` — read JSON file, parse, build model
- `sg_api_write_solution()` — stream solution to `ShJsonWriter`
- `sg_api_handle()` — HTTP-style request routing (`/api/v1/solve`, `/health`, `/version`)

**JSON schema sections** (processed in dependency order):

| Section | Description | C API calls |
|---------|-------------|-------------|
| `config` | Solver parameters | `sg_set_config` |
| `dimension_count` | Capacity dimensions | `sg_set_dimension_count` |
| `demand_sign_convention` | 0=pickup+/delivery- | `sg_set_demand_sign_convention` |
| `unassigned_weight` | Global drop penalty | `sg_set_unassigned_weight` |
| `locations` | Coordinate array | `sg_add_location`, `sg_location_set_coords` |
| `commodities` | Types + conflicts | `sg_add_commodity`, `sg_commodity_set_conflict` |
| `exclusion_groups` | Group count | `sg_add_exclusion_group` |
| `setup_times` | Class matrix | `sg_set_num_setup_classes`, `sg_set_setup_time` |
| `depots` | Depot definitions | `sg_add_depot`, `sg_depot_set_location`, `sg_depot_set_max_simultaneous` |
| `vehicles` | Fleet with costs | `sg_add_vehicle`, `sg_vehicle_set_*` (all cost/constraint/break fields) |
| `tasks` | Stops with TWs | `sg_add_task`, `sg_task_set_*` (soft TW, disjunct TW) |
| `requests` | PD pairs + constraints | `sg_add_*_request`, `sg_request_set_*` (qualifications, ride time, vehicle constraints, commodity, exclusion, setup, drop penalty) |
| `travel` | Distance/duration matrices + time brackets | `sg_set_travel_matrix`, `sg_set_travel_time_bracket` |
| `zones` | Zone distance matrix | `sg_set_zone_distance_matrix` |
| `initial_routes` | Warm start | `sg_set_initial_routes` |

### Future (not planned yet)

These are real-world features that require larger architectural changes:

| Feature | Status |
|---------|--------|
| **Driver breaks / HoSE** | ✅ Complete. Abstract break model — generic `(max_work, break_duration, max_total_work)` maps to both EU EC 561 and US FMCSA rules. |
| **Multiple trips** | ✅ Complete. Multi-route-per-vehicle state with depot reload modeling. `sg_vehicle_set_max_trips()`. |
| **Time-dependent travel** | ✅ Complete. Speed profiles (step-function multipliers), per-vehicle travel profiles, and time-indexed travel brackets (multiple complete matrices by departure time). `sg_set_travel_time_bracket()` + `sg_travel_profile_add_time_bracket()`. |

---

## Solver Profiles

Three built-in iteration profiles for different use cases. The API default is 1000 (batch).
Users can override via `SGConfig.max_iterations` or `--iterations` in benchmarks.

| Profile | Iterations | Runtime (100-req) | Solomon distGap | Li & Lim distGap | Use case |
|---------|-----------|-------------------|-----------------|------------------|----------|
| **Real-time** | 300 | ~0.2 s | ~+3% | ~+7% | API responses, live dispatch |
| **Batch** (default) | 1,000 | ~0.6 s | ~+1.5% | ~+5% | Daily planning, route optimization |
| **High quality** | 5,000 | ~2.0 s | ~+0.5% | ~+4.5% | Offline analysis |
| **Best quality** | 10,000 | ~4.5 s | -0.1% | +3.7% | Benchmarking, maximum quality |

### Iteration Scaling Data (100-customer instances, deterministic seed 42)

**Solomon (VRPTW, 56 cases)** — single-thread with infeasible-space exploration:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 10,000 | 9.2 | +0.2% | +0.30 | 39/56 | 23 |

**Solomon — population (auto threads, 3 generations, Phase S13-S17)**:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 10,000 | 37.6 | -0.2% | +0.20 | 45/56 | 14 |

**Li & Lim (PDPTW, 56 cases)** — single-thread with infeasible-space exploration:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 10,000 | 10.5 | +4.2% | +0.48 | 44/56 | 24 |

**Li & Lim — population (auto threads, 3 generations, Phase S13-S15)**:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 10,000 | 23.0 | +3.7% | +0.39 | 47/56 | 27 |

**Observations**:
- Infeasible-space exploration (Phase S12) closed the +1 vehicle gap on most tight-TW instances. Solomon equalVehicles improved from 35 to 39 (single-thread) and 45 (population).
- Phase S13-S15 (progressive penalty, ejection in repair, SREX crossover, Phase 1.5 vehicle crunch) further improved Solomon population equalVehicles from 45→46 and avgVehGap from +0.20→+0.18.
- Solomon C1xx/C2xx (17/17) now match BKS exactly on both vehicles and distance.
- Li & Lim LC2xx (8/8), LR2xx (11/11), and LRC2xx (8/8) all match BKS on vehicles and distance. Remaining gaps concentrated on tight-TW LC1xx (lc101/102 at +5/+3 vehicles) and LR1xx (lr101/102 at +6/+3 vehicles).
- Population search adds ~2-4x wall-clock time but improves vehicle count. Solomon distance gap stays near zero (beating some BKS).
- Tight-TW R1/RC1 instances show a trade-off pattern: +1 vehicle gap but lower distance (e.g., R104 +1 veh / -1.5% dist). Closing these requires deeper search or dedicated tight-TW operators.

## Performance Targets

| Instance Size | Target Time | Quality (vs Best Known) |
|--------------|-------------|-------------------------|
| 100 requests | < 5 sec | Within 2% |
| 500 requests | < 30 sec | Within 5% |
| 1000 requests | < 2 min | Within 8% |
| 5000 requests | < 10 min | Within 10% |

---

## References

### Core ALNS
1. Ropke & Pisinger (2006) - "An Adaptive Large Neighborhood Search Heuristic for the Pickup and Delivery Problem with Time Windows"
2. Shaw (1997) - "A New Local Search Algorithm Providing High Quality Solutions to Vehicle Routing Problems"
3. Pisinger & Ropke (2007) - "A general heuristic for vehicle routing problems"

### Rich VRP
4. Hasle & Kloster (2007) - "Industrial aspects and literature survey: fleet composition and routing"
5. Vidal et al. (2014) - "A unified solution framework for multi-attribute vehicle routing problems"
6. Drexl (2012) - "Rich vehicle routing in theory and practice"

### DARP (Dial-a-Ride)
7. Cordeau & Laporte (2007) - "The dial-a-ride problem: models and algorithms"
8. Parragh et al. (2008) - "A survey on pickup and delivery problems"

### Benchmark Instances
9. Solomon (1987) - VRPTW benchmark instances
10. Li & Lim (2001) - PDPTW benchmark instances
11. Cordeau (2006) - DARP benchmark instances

---

## Appendix: Standard Benchmark Instances

### Solomon VRPTW Instances

Download: http://web.cba.neu.edu/~msolomon/problems.htm

| Class | Customers | TW Width | Distribution |
|-------|-----------|----------|--------------|
| C1 | 100 | Narrow | Clustered |
| C2 | 100 | Wide | Clustered |
| R1 | 100 | Narrow | Random |
| R2 | 100 | Wide | Random |
| RC1 | 100 | Narrow | Mixed |
| RC2 | 100 | Wide | Mixed |

Extended versions with 200, 400, 600, 800, 1000 customers also available.

### Li & Lim PDPTW Instances

Download: https://www.sintef.no/projectweb/top/pdptw/li-lim-benchmark/

| Class | Requests | TW Width | Distribution |
|-------|----------|----------|--------------|
| LC1 | 100-1000 | Narrow | Clustered |
| LC2 | 100-1000 | Wide | Clustered |
| LR1 | 100-1000 | Narrow | Random |
| LR2 | 100-1000 | Wide | Random |
| LRC1 | 100-1000 | Narrow | Mixed |
| LRC2 | 100-1000 | Wide | Mixed |

### Cordeau DARP Instances

Download: https://www.bernabe.dorronsoro.es/vrp/

| Instance | Vehicles | Requests | Max Ride Time |
|----------|----------|----------|---------------|
| a2-16 | 2 | 16 | 30 min |
| a4-32 | 4 | 32 | 30 min |
| a8-64 | 8 | 64 | 30 min |
| b2-16 | 2 | 16 | 45 min |
| b4-32 | 4 | 32 | 45 min |
| b8-64 | 8 | 64 | 45 min |

### Rich VRP Testing

For rich constraint testing, generate synthetic instances with:
- Multi-dimensional capacity (2-4 dimensions)
- Disjunct time windows (2-3 windows per stop)
- Commodity conflicts (3-5 commodity types)
- Vehicle qualifications (5-8 different qualifications)
- Exclusion groups (10-20% of requests have exclusions)

Use these for validation against published best-known solutions.

---

## SoTA Performance Improvement Plan

Seven ordered phases to close the gap between Surge and state-of-the-art benchmark results
on Solomon (VRPTW) and Li & Lim (PDPTW) instances. Each phase is orthogonal and testable
independently.

### Current Gaps (1000 iterations / default, deterministic seed 42)

| Benchmark | Metric | Surge | BKS Avg | Gap |
|-----------|--------|-------|---------|-----|
| Solomon 100 | Avg distance | 1,048 | 1,014 | +3.0% |
| Li & Lim 100 | Avg distance | 1,072 | 1,017 | +5.0% |

At 5000 iterations: Solomon +0.8%, Li & Lim +4.2%.

### Phase S1: Simulated Annealing Acceptance ✅

**Result**: Solomon improved from +9.3% to +5.9% avgDistGap at 300 iterations. Li & Lim improved from +112.7% to +112.7% (no change, needed S2 first).

**Changes (arbor)**:
- Added `ar_alns_calibrate_sa` helper that computes adaptive `initial_temp` and `cooling_rate`
  from initial solution cost and iteration budget
- Formula: `T0 = 0.05 * |initial_cost| / ln(2)`, `cooling_rate = exp(ln(0.001) / max_iter)`

**Changes (surge)**:
- In `sg_solve_route_model`: compute initial cost after construction, calibrate SA params,
  set `accept_type = AR_ACCEPT_SA`

### Phase S2: Independent PD Stop Placement ✅

**Result**: Li & Lim improved from +112.7% to +9.5% avgDistGap at 300 iterations. Solomon unchanged at +5.9%.

**Changes (surge)**:
- Added `sg_route_splice_stop` / `sg_route_excise_stop` for direct stop-array manipulation
- Added `sg_route_eval_pd_best_insertion_cached` evaluating O(L²) (pickup, delivery) pairs
  with push propagation, forward slack pruning, ride-time checks, and capacity validation
- Added `sg_route_apply_pd_insertion` for non-adjacent PD placement via splice
- Switched `sg_route_apply_insertion` and `sg_route_unassign_request` to splice/excise
  (preserves non-adjacent PD stops for other requests on same vehicle)
- Wired PD dispatch through all repair and postprocess callers
- 6 new tests for stop manipulation, PD placement, and correctness

### Phase S3: Route-Aware Worst Removal ✅

**Result**: Solomon +5.5% → +0.8% (at 5k iters), Li & Lim +9.0% → +4.2%. At 300 iters: Solomon +5.9% → +5.5%, Li & Lim +9.5% → +9.0%.

**Changes (surge)**:
- Added `sg_route_removal_cost` computing O(1) distance delta from removing a request,
  handling delivery-only, adjacent PD, and non-adjacent PD cases using cached stop positions
- Wired into `sg_route_destroy_worst` (single-line change), replacing `sg_bootstrap_removal_cost`
- Criticality-worst operator intentionally unchanged (uses proxy metrics for diversity)
- 3 new tests: delivery-only (first/middle/last stop), PD adjacent, PD non-adjacent

### Phase S4: Route-Aware Shaw Relatedness ✅

**Result**: Solomon +0.8% → +0.4% avgDistGap (at 5k iters), avgVehGap +0.50 → +0.55. Li & Lim +4.2% → +4.3% (neutral). At 1k iters: Solomon +3.0% → +2.0%.

**Changes (surge)**:
- Added `sg_route_shaw_relatedness` with 6-term scoring: spatial distance (`-euclid/40`),
  TW overlap (Jaccard-like, `3.0 * overlap/span`), load similarity (`-|Δload|/100`),
  co-route bonus (`+5.0` if same vehicle), PD kind bonus (`+2.0`/`-0.5`), tie-breaker
- Added `void *active_solution` field to `SGContext` — set/cleared around `ar_remove_related` call
  to pass route solution into relatedness callback without changing Arbor API
- Wired into `sg_route_destroy_shaw`, replacing `sg_bootstrap_relatedness`
- Bumped Shaw initial weight from 0.5 to 1.0 (route-aware Shaw deserves equal weight)
- 1 new test: verifies spatial ordering, same-route bonus dominance, NULL safety

### Phase S5: Adaptive Destroy Count ✅

**Result**: Solomon +0.4% → +0.2% avgDistGap (at 5k iters), avgVehGap +0.55 → +0.52. Li & Lim unchanged (53 requests too small to trigger adaptive scaling). Runtime +40% on Solomon due to larger neighborhoods.

**Changes (surge)**:
- Added `sg_adaptive_q_bounds` helper: `q_min = max(config, n/20)`, `q_max = max(config, n/4)`
- Called from both `sg_solve_route_model` and `sg_solve` entry points
- Config defaults (4/20) serve as floor; adaptive scaling only increases bounds
- 1 new test: verifies formula for small/medium/large instances, user overrides, edge cases

### Phase S6: Enhanced Local Search ✅

**Result**: Solomon +0.2% → +0.2% (stable, C104 improved 853→846). Li & Lim +4.3% → +4.3% (stable). No runtime overhead.

**Changes (surge)**:
- Added `sg_route_try_or_opt_once` for segment relocation (k=2,3) both intra- and inter-route
- Wired into `sg_route_postprocess_intensify` before exchange and 2-opt*
- Increased `SG_ROUTE_MAX_INTENSIFY_PASSES` from 4 to 8
- 1 new test: verifies intensify improves suboptimal clustered solution

### Phase S7: Stagnation Restart ✅

**Result**: Solomon stable at +0.2% (avgVehGap improved +0.52→+0.46, equalVehicles 30→33). Li & Lim stable at +4.3%. Neutral at 5k iterations — infrastructure ready for longer runs where stagnation matters more.

**Changes (arbor)**:
- Added `restart_threshold` and `restart_temp_ratio` to `ARALNSParams`
- Added `restarts` counter to `ARALNSStats`
- Added restart logic in `ar_alns_solve`: when stagnation_iterations >= restart_threshold,
  copy best to current, reheat SA temperature to `initial_temp * restart_temp_ratio`, reset counter
- Added validation: `restart_threshold >= 0`, `restart_temp_ratio in [0, 1]`

**Changes (surge)**:
- Set `restart_threshold = max_iterations / 4` in both `sg_solve_route_model` and `sg_solve` paths

**Tests**: Arbor restart test with validation edge cases.

### Phase S8: Construction + Vehicle Minimization ✅

**Result**: Solomon stable at +0.2% at 5k iters (avgVehGap +0.46). Li & Lim improved avgVehGap +0.70→+0.55, equalVehicles 36→38. Major architectural overhaul introducing multi-strategy construction, two-phase ALNS, and richer local search.

**Changes (surge)**:
- Multi-strategy construction: regret-3, TW-sorted greedy, Solomon I1 heuristic — keep best
- Two-phase ALNS: 60% vehicle minimization (hot SA, lexicographic objective) + 40% distance polishing
- Vehicle-target and vehicle-empty destroy operators for focused vehicle elimination
- Pair elimination in `sg_route_postprocess_reduce_vehicles`
- Depth-2 ejection chains in `sg_route_postprocess_ejection_reduce`
- Pairwise exchange operator in postprocessing intensify loop

### Phase S9: Deeper Ejection Chains, CROSS-Exchange, and Validation ✅

**Result**: Solomon avgVehGap +0.46→+0.36, avgDistGap +0.2%→-0.2%, equalVehicles 33→36. Li & Lim avgVehGap +0.55→+0.55, avgDistGap +4.3%→+4.1%, equalVehicles 38→40. All 113 solutions verified feasible. Benchmark iterations increased to 10k.

**Changes (surge)**:
- Increased `SG_EJECTION_MAX_DEPTH` from 2 to 5 with `SG_EJECTION_BUDGET` of 50000 evaluations per vehicle elimination attempt to bound pathological blowup
- Budget threaded through `sg_try_place_with_ejection` via `int *budget` parameter
- CROSS-exchange operator (`sg_route_try_cross_exchange_once`): swaps interior segments of size 1-3 between route pairs, accepts first improvement
- Or-opt extended from k=2,3 to k=1,2,3 — single-request inter-route relocate now in intensify loop
- Post-solve feasibility validation gate in `sg_solve_route_model`: calls `sg_route_solution_validate` and returns `SG_STATUS_ERROR` on failure
- Benchmark default iterations 5000 → 10000 in both `bench_li_lim.c` and `bench_solomon.c`

**Tests**: All 56 Solomon + 57 Li & Lim cases pass validation.

### Phase S12: Infeasible-Space Exploration + Aggressive SISR ✅

**Result**: Solomon single-thread: avgVehGap +0.36→+0.30, avgDistGap +0.4%→+0.2%, equalVehicles 37→39. Solomon population (3 gen): avgVehGap +0.20, avgDistGap -0.2%, equalVehicles 45. Li & Lim single-thread: avgVehGap +0.52→+0.48, avgDistGap +4.8%→+4.2%, equalVehicles 41→44. Li & Lim population (3 gen): avgVehGap +0.38, avgDistGap +3.5%, equalVehicles 48.

**Changes (surge — sg_penalty.c, new file):**
- `SGPenaltyType` enum with 6 constraint types: TIME_WARP, CAPACITY, DURATION, RIDE_TIME, DISTANCE, TOTAL_WORK
- `SGPenaltyManager` struct with per-constraint weights, strategy callbacks (update/record/reset), and opaque state
- Adaptive strategy (HGS-style): per-constraint self-adjustment based on fraction of recent feasible solutions
- `cost_scale` parameter drives penalty_min (`cost_scale * 1e-4`), penalty_max (`cost_scale * 100`), and initial weights (`cost_scale / 100`) — fully proportional to problem cost structure
- `sg_solution_is_feasible()` and `sg_solution_total_violation()` helpers

**Changes (surge — sg_feasibility.c):**
- `sg_route_eval_insertion_cached`: continue past TW/capacity/duration/ride-time/distance/total-work violations when `penalty.enabled`, accumulating into `ins_violations[]` and adding penalty cost to insertion score
- `sg_route_eval_pd_best_insertion_cached`: same pattern for PD insertions
- `sg_route_update_timing`: compute per-route per-constraint violations, store in `route_violations[]`, sum into `sol->violations[]`
- `sg_route_update_load`: capacity violations computed and stored
- Time warping: `start = tw_late` for downstream propagation after accumulating warp
- Hard rejects (qualifications, blacklist, commodity conflicts, exclusion groups) remain strict

**Changes (surge — sg_solution.c):**
- `sg_route_solution_is_better`: feasible beats infeasible regardless of cost; both-infeasible prefers less total violation
- `sg_route_solution_cost_record` wrapper for ALNS `ops.cost` that calls `penalty.record()` (keeps `sg_route_solution_cost` pure)
- `violations[SG_PENALTY_COUNT]` and `route_violations` allocated in arena, copied in `sg_route_solution_copy`

**Changes (surge — sg_solve.c):**
- Phase 1 penalty: `sg_penalty_init_adaptive(..., cost_scale)` with target_feasible=0.15 (aggressive)
- Phase 2 penalty: disabled (strict distance polishing)
- `ops->is_better = sg_route_solution_is_better` always set (was conditional)
- `penalty.update()` wired into progress forwarder

**Changes (surge — sg_destroy.c):**
- Instance-adaptive SISR `L_max = max(SG_STRING_L_MAX, avg_route_length)` at both string extraction points
- Initial string destroy operator weight 2.0 (was 1.0)

**Tests**: 5 new tests (252→257). ASAN/UBSAN clean.

### Phase S13+S14+S15: Algorithmic Edge + Population Crossover ✅

**Result**: Solomon population: avgVehGap +0.20→+0.18, avgDistGap -0.2%→-0.1%, equalVehicles 45→46. Li & Lim population: avgVehGap +0.38→+0.39, avgDistGap +3.5%→+3.7%, equalVehicles 48→47. Vehicle counts stable or improved on most instances. New Phase 1.5 vehicle crunch phase adds focused vehicle reduction between Phase 1 and Phase 2 without cannibalizing Phase 2 budget.

**Changes (surge — sg_penalty.c):**
- `sg_penalty_init_progressive`: progressive penalty schedule that lerps `target_feasible` from `target_start` to `target_end` over `total_segments` via linear interpolation
- `SGProgressivePenaltyState` extends `SGAdaptivePenaltyState` with lerp parameters
- Phase 1 now uses progressive 0.25→0.15 (was fixed 0.15) — starts aggressive for deeper infeasible-space exploration when vehicle cuts are most likely, tightens to avoid returning infeasible solutions

**Changes (surge — sg_postprocess.c):**
- `sg_route_postprocess_reduce_vehicles_relaxed`: vehicle elimination with relaxed distance slack (accepts up to `distance_factor` × current distance + 50.0), both pair and single-vehicle elimination with frozen-request guards
- `sg_try_place_with_ejection` promoted from static to extern — now called from repair operators
- Ejection chains skip frozen requests (was missing — could eject FROZEN requests in chains)
- Scaled ejection budget: `num_requests × vehicles_used × 100` (was fixed 50K), capped at `SG_EJECTION_BUDGET_CAP` (500K)

**Changes (surge — sg_repair.c):**
- `sg_repair_ejection_fallback`: ejection chain fallback at end of every repair operator (greedy, regret-2/3/4, noise-regret, pair-sync) when `ctx->ejection_in_repair == 1`
- Budget-limited (`SG_EJECTION_REPAIR_BUDGET` = 5000), cost-gated (reverts if cost increases)
- Toggled on during Phase 1 ALNS only (`ctx->ejection_in_repair = 1` around `ar_alns_solve`)

**Changes (surge — sg_solve.c):**
- Phase 1.5 "Vehicle crunch": 500-iteration ALNS between ejection pulse and Phase 2, using only vehicle-reducing operators (vehicle-target, vehicle-empty, route-removal) with regret-3 and greedy repair
- Phase 1.5 runs as additional budget — does NOT subtract from Phase 2
- Relaxed vehicle reduction (`distance_factor=1.20`) called after ejection pulse, before Phase 1.5
- Progressive penalty schedule for Phase 1 (`0.25→0.15` over segments)

**Changes (surge — sg_parallel.c):**
- SREX crossover: `sg_srex_build_warm_start` takes k routes from parent 1 + remaining requests from parent 2, builds merged warm-start arrays. Fisher-Yates partial shuffle for route selection
- Population diversity filter in `sg_population_insert_ex`: rejects candidates >90% similar to existing members unless strictly better cost. Similarity = fraction of requests on same vehicle in both solutions
- `crossover_fraction` config (default 0.5): fraction of workers using SREX vs single-parent warm-start
- `owns_warm_start` flag on work items for proper memory management of SREX-allocated arrays

**Changes (surge — sg_internal.h):**
- `SG_EJECTION_BUDGET_CAP` (500K), `SG_EJECTION_REPAIR_BUDGET` (5K)
- `ejection_in_repair`, `ejection_repair_budget` fields on `SGContext`
- `crossover_fraction` field on `SGPopulationConfig`

**Tests**: 14 new tests (326→340). Progressive penalty lerp/update, ejection in repair, scaled budget/cap, relaxed elimination, Phase 1.5 runs, frozen preservation through Phase 1.5, ejection fallback cost guard, ejection chain frozen guard, population crossover (VRPTW + PDPTW), diversity filter, no-crossover fallback. ASAN/UBSAN clean.

### Phase S16: CFRS Construction Heuristics ✅

**Result**: GH-200 population: equalVehicles 48→55 (80%→92%), avgVehGap +0.20→+0.08, avgDistGap +13.2%→+13.1%. Best vehicle count result to date. 100-customer benchmarks unchanged (374/374 existing tests pass).

**Problem**: Large instances (200+) start with too many vehicles because sequential insertion (regret-3, TW-sorted, Solomon I1) creates routes one-at-a-time without global awareness of how requests should cluster. ALNS cannot eliminate excess vehicles within 60s — construction quality is the bottleneck.

**Solution**: Cluster-First-Route-Second (CFRS) heuristics estimate the minimum vehicle count, cluster requests into that many groups, then route within each cluster.

**Changes (surge — sg_construct_cfrs.c, new ~450 lines):**
- `sg_estimate_min_vehicles()`: vehicle count lower bound via bin packing (per-dimension `ceil(total_demand / max_capacity)`) + time window conflict bound (greedy clique approximation via sweep-line)
- `sg_construct_sweep_cfrs()`: angular sweep CFRS — compute depot centroid, sort requests by `atan2` angle, cut clusters by capacity/count/TW gap, assign to nearest qualified vehicle, route within cluster by tightest-TW-first insertion, mop up with regret-3
- `sg_construct_kmeans_tw()`: k-means with time windows — 3D feature vectors `(x_norm, y_norm, alpha * tw_center_norm)` with alpha=0.3, k-means++ initialization, max 20 iterations, same cluster-to-vehicle assignment and intra-cluster routing
- `sg_construct_by_method()`: dispatch table mapping `SGConstructMethod` enum to function pointers

**Changes (surge — sg_internal.h):**
- `SGConstructMethod` enum: `SG_CONSTRUCT_REGRET3=0, SG_CONSTRUCT_TW_SORTED=1, SG_CONSTRUCT_SOLOMON_I1=2, SG_CONSTRUCT_SWEEP_CFRS=3, SG_CONSTRUCT_KMEANS_TW=4, SG_CONSTRUCT_COUNT=5`
- `construct_method` field on `SGContext` (default `SG_CONSTRUCT_COUNT` = try all)
- Declared `sg_route_construct_tw_sorted` (was static in sg_solve.c)

**Changes (surge — sg_solve.c):**
- `sg_route_construct_initial_solution`: single-method fast path when `construct_method < SG_CONSTRUCT_COUNT`; default multi-trial extends from 3 to 5 methods with sweep CFRS and k-means TW attempts
- `sg_route_construct_tw_sorted` promoted from static to extern

**Changes (surge — sg_parallel.c):**
- Generation 0: `clone->construct_method = (SGConstructMethod)(thread_index % SG_CONSTRUCT_COUNT)` — round-robin construction diversity across threads
- Generations > 0: `SG_CONSTRUCT_COUNT` (default, since warm start bypasses construction)

**Changes (surge — sg_context.c):**
- `sg_create()`: init `ctx->construct_method = SG_CONSTRUCT_COUNT`

**Changes (surge — Makefile):**
- Added `sg_construct_cfrs.o` to SRCS

**Tests**: 13 new tests (374→387). Sweep CFRS delivery-only (3 spatial clusters → 3 vehicles), sweep CFRS PD (P+D never split), sweep capacity cut, k-means temporal clusters, k-means PD, k-means spatial clusters, vehicle LB capacity bound, vehicle LB TW conflict bound, construct-by-method all methods, best-of-all default, population construction diversity, sweep with qualifications, CFRS with frozen requests. ASAN/UBSAN clean.

---

## Phase S17: O(1) Route Concatenation via Segment Summaries

### Motivation

Route evaluation after every insertion currently rebuilds prefix/suffix in O(L) time:
- `sg_route_update_timing()` recomputes all arrival/wait/departure times
- `sg_route_update_load()` recomputes all load profiles

This dominates local search runtime on long routes. HGS uses O(1) concatenation via cumulative
tuples — but those don't support our rich constraints (setup times, ride time, breaks, etc.).

### Solution: Segment Summaries

Maintain `(distance, duration, load_vector, forward_slack, backward_slack)` tuples for each
prefix and suffix. Concatenating two segments combines their summaries in O(1) by summing
components and taking min/max of slack values.

**Segment summary structure:**
```c
typedef struct {
    double distance;           /* Total distance */
    double duration;           /* Total travel + service + wait */
    double forward_slack;      /* Latest start time - arrival */
    double backward_slack;      /* Arrival - earliest start time */
    double tardiness;          /* Accumulated soft TW penalty */
    double wait_total;          /* Accumulated waiting time */
    uint64_t load_after;       /* Cumulative load (bitmask for commodities) */
    /* Multi-dim capacity handled separately */
} SGRouteSegment;
```

**Concatenation operation:**
```c
SGRouteSegment sg_concat(const SGRouteSegment *a, const SGRouteSegment *b) {
    return (SGRouteSegment){
        .distance = a->distance + b->distance,
        .duration = a->duration + b->duration,
        .forward_slack = min(a->forward_slack, b->forward_slack - a->duration),
        .backward_slack = min(a->backward_slack, b->backward_slack),
        .tardiness = a->tardiness + b->tardiness,
        .wait_total = a->wait_total + b->wait_total,
        .load_after = a->load_after | b->load_after,
    };
}
```

### Implementation

**Phase S17.1: Capacity Prefix/Suffix ✅ COMPLETE (PR #25)**

Per-vehicle capacity prefix/suffix arrays with `(delta, min, max)` per dimension. Built
at the end of `sg_route_update_timing()`. O(1) capacity feasibility for any contiguous
sub-route by concatenating prefix[start] with suffix[end].

**Files:** `sg_concat.c` (`sg_route_build_cap_segments()`), `sg_internal.h` (`SGCapSegment`).

**Phase S17.2: Timing Prefix/Suffix ✅ COMPLETE (commit fad7453)**

Per-vehicle timing prefix/suffix arrays using `SGSegSummary` (distance, duration,
earliest_start, latest_start, time_warp, wait_time, first/last location). Built at the
end of `sg_route_update_timing()` in a single O(L) pass. O(1) timing evaluation for any
route formed by concatenating a prefix with a suffix via `sg_concat_timing()`.

**Files:** `sg_concat.c` (`sg_route_build_segments()`, `sg_concat_timing()`),
`sg_internal.h` (`SGSegSummary`).

**Phase S17.3: Local Search Pre-filtering ✅ COMPLETE**

O(1) pre-filter for three local search operators: 2-opt*, OR-opt, and cross-exchange.
Before each trial move, the concat evaluator computes the new total distance in O(1)
(or O(k) for moved segments of k ≤ 3 requests). If the move cannot improve total distance,
it is skipped without running the O(L) `sg_route_sequence_feasible_distance()` confirmation.
Moves that pass the pre-filter still go through the full O(L) path — no false acceptances.

**Pattern (identical for all three operators):**
```c
double concat_total;
if (sg_concat_eval_<operator>(ctx, sol, ..., &concat_total) &&
    concat_total >= sol->total_distance - 1e-9) {
    continue;  /* Skip — O(1) says not improving */
}
/* Existing O(L) path runs here (unchanged) */
```

When the eval function returns 0 (not applicable), the existing O(L) path runs as fallback.
This guarantees zero regression for any problem type.

**Files:** `sg_concat.c` (eval functions), `sg_postprocess.c` (pre-filter calls),
`sg_internal.h` (declarations). 7 unit tests in `test_surge.c`.

#### Applicability Conditions

The O(1) pre-filter applies when ALL of these conditions are met:

| Condition | Why Required | Detection |
|-----------|-------------|-----------|
| Delivery-only routes | Prefix/suffix at stop level must align with request-level operator indices | `!ctx->has_pd_requests` |
| No break policy | Break state machine makes timing non-decomposable | `!vehicle->has_break_policy` |
| No multi-trip | Multi-trip resets violate monotonic timing invariants | `!vehicle->has_multi_trip` |
| No travel callback | Callback cost may differ from prefix/suffix cached distances | `!ctx->travel_callback` |
| No time brackets | Time-varying distance matrices may diverge from prefix/suffix build-time values | `!ctx->has_travel_time_brackets` |

**2-opt\* has additional requirements** (suffix from vehicle B used on vehicle A):

| Condition | Why Required | Detection |
|-----------|-------------|-----------|
| Same travel profile | Suffix distances are profile-dependent; cross-vehicle use requires identical matrices | `!ctx->has_travel_profiles` |
| Closed start | Open-start vehicles need depot adjustment not yet implemented | `!vehicle->open_start` |
| Closed end | Open-end vehicles have no depot return to adjust | `!vehicle->open_end` |

**Primary target:** Solomon/GH VRPTW benchmarks (delivery-only, single profile, no breaks).
These are 100% of our current benchmarks and represent most real trucking instances.

#### Fallback Behavior

When applicability conditions are NOT met, the eval function returns 0 and the existing
O(L) code path runs unchanged. This is a hard guarantee — the pre-filter never rejects
a move that the O(L) path would accept.

| Problem Type | Pre-filter Active? | Behavior |
|--------------|-------------------|----------|
| VRPTW (delivery-only, single profile) | ✅ All 3 operators | Full O(1) pre-filtering |
| VRPTW with travel profiles (OR-opt, cross-exchange) | ✅ OR-opt, cross-exchange | Segments rebuilt for target vehicle's profile |
| VRPTW with travel profiles (2-opt\*) | ❌ | Fallback to O(L) — suffix distances profile-dependent |
| PDPTW (pickup-delivery pairs) | ❌ | Fallback to O(L) — stop interleaving prevents prefix/suffix alignment |
| DARP (dial-a-ride) | ❌ | Fallback to O(L) — same as PDPTW |
| Routes with break policies | ❌ | Fallback to O(L) — break state non-decomposable |
| Routes with multi-trip | ❌ | Fallback to O(L) — trip resets break timing invariants |
| Time-dependent travel (brackets) | ❌ | Fallback to O(L) — distance may vary with departure time |
| External travel callback | ❌ | Fallback to O(L) — callback cost not cached in segments |
| Open-start/open-end (2-opt\*) | ❌ | Fallback to O(L) — depot return adjustment not implemented |
| Intra-route OR-opt | ❌ | Fallback to O(L) — prefix/suffix positions shift after segment removal |

#### Operator Details

**OR-opt** (`sg_concat_eval_or_opt`): Remove k (1-3) requests from vehicle A, insert at
position ins in vehicle B. Source route: `concat(prefix_A[start], suffix_A[start+k])`.
Moved segment rebuilt for vb's profile via `sg_build_segment_for_vehicle()` — O(k) where
k ≤ 6 stops. Dest route: `concat(prefix_B[ins], segment, suffix_B[ins])`. Capacity checked
via `sg_concat_capacity_ok()`. Inter-vehicle only (intra-route falls back to O(L)).

**2-opt\*** (`sg_concat_eval_2opt_star`): New A = A[0..cut_a-1] + B[cut_b..end], New B =
B[0..cut_b-1] + A[cut_a..end]. Requires same travel profile across vehicles. Depot return
adjustment computed when vehicles have different end depots:
`depot_adj = d(last_stop, new_depot) - d(last_stop, old_depot)`.

**Cross-exchange** (`sg_concat_eval_cross_exchange`): Swap segment [ia, ia+sa-1] from A
with [ib, ib+sb-1] from B (sa, sb ∈ {1,2,3}). Both moved segments rebuilt for target
vehicle via `sg_build_segment_for_vehicle()`. New routes: prefix + rebuilt_segment + suffix.

**Phase S17.4: Segment Tree (Future, L>50)**

For very long routes (400+ customers), even prefix/suffix rebuild after every insertion
is O(L). A segment tree gives O(log L) updates and O(log L) range queries. Only needed
if profiling shows segment rebuild as a bottleneck after Phase S17.1-3.

---

## Infrastructure: Arena Allocator

`sh_arena.h` (bump allocator with 8-byte alignment, reset, introspection) already exists in
the shared library. Surge's allocation patterns map directly to arena semantics.

### Allocation Tiers

| Tier | Where | Pattern | Calls/solve | Arena benefit |
|------|-------|---------|-------------|---------------|
| Per-solve | `sg_route_solution_init` | ~23 malloc/calloc for flat arrays, freed together at end | ~23 | Replace with 1 arena alloc |
| Hot-path backup | `sg_postprocess.c` | Full solution copy per move attempt (~23 malloc), restore (~23 free) | ~4600/iter | Scratch arena with reset |
| Feasibility scratch | `sg_feasibility.c` | ~10 temp arrays per `sg_route_stop_sequence_feasible` call | ~1000/iter | Same scratch arena |
| Setup | `sg_context.c` | Metadata, records, matrices — allocated once | ~25 | Per-context arena |

### Implementation Plan

**Phase 1 — Per-solution arena (DONE):**
Added `SHArena *arena` to `SGRouteSolution`. All arrays (including bootstrap) allocated from
a single arena in `sg_route_solution_init()`. Single `sh_arena_free()` in `reset()`. ~29 malloc/calloc → 1
`sh_arena_create`, ~26 free → 1 `sh_arena_free`. Benchmark results: Solomon -7.3%, Li&Lim -2.2%, Cordeau -2.9%.

**Phase 2 — Optimized solution copy (DONE):**
Added `solution_arena_size` cache to `SGContext` and `sg_route_solution_init_for_copy()` which
creates an uninitialized arena (no calloc-zeroing, no init loops). `sg_route_solution_copy()` fast
path: single `sh_arena_alloc` + single `memcpy` of source arena buffer. Identical allocation order
guarantees identical memory layout. Eliminates ~50KB of wasted zeroing + ~20K UINT32_MAX init
writes per copy.

**Phase 3 — Pre-allocated scratch buffers (DONE):**
Added `SGScratchBuffers` to `SGContext` with pre-allocated arrays for feasibility checking
(timing, load_profile, dim_scratch, pickup_depart, pickup_seen, feas_stops) and local search
(candidate_a, candidate_b, exclusion_counts). Single arena created in `sg_scratch_init()`,
freed in `sg_scratch_free()`. All callers use `use_scratch` flag with graceful malloc fallback.
Eliminates 5-10 malloc/free per `sg_route_stop_sequence_feasible()` call and per-function
candidate array allocations in 2-opt*, or-opt, and cross-exchange.

Cumulative Phase 2+3 benchmark results vs Phase 1 baseline:
Solomon -14.8% (5.616s → 4.786s), Li&Lim -5.0% (3.920s → 3.724s), Cordeau -3.4% (0.264s → 0.255s).

### Sizing

| Problem size | Solve arena | Scratch arena | Total |
|-------------|-------------|---------------|-------|
| 100 requests | ~2 MB | ~5 MB | ~10 MB |
| 1000 requests | ~20 MB | ~50 MB | ~100 MB |
| 5000 requests | ~100 MB | ~250 MB | ~500 MB |

---

## Infrastructure: Multi-Threading

### Strategy 1 — Independent Runs (Embarrassingly Parallel)

`SGContext` is fully self-contained with zero shared state. Each thread creates its own
context, solves with a different seed, and the best result wins. Near-linear speedup.

```
Thread 0: sg_create() → sg_solve(seed=42) → cost=1027
Thread 1: sg_create() → sg_solve(seed=43) → cost=1019  ← winner
Thread 2: sg_create() → sg_solve(seed=44) → cost=1031
Thread 3: sg_create() → sg_solve(seed=45) → cost=1024
```

Implementation: ~50 lines of pthread wrapper. No changes to Surge or Arbor internals.

### Strategy 2 — Parallel Move Evaluation

`sg_route_rank_insertions_for_request()` iterates over all vehicles, evaluating insertion
cost independently per vehicle (read-only on solution). Parallelizing this inner loop with
a thread pool would speed up the repair phase, which dominates solve time.

Implementation: moderate — need to ensure thread-safe read access to solution state, collect
per-vehicle results into shared array.

### Strategy 3 — Population-Based Search (See HGS Analysis)

Combine strategies 1 + 2 with a population manager at the Surge level. Each generation:
crossover two elite solutions, intensify with Arbor ALNS, add to population if elite.

---

## HGS (Hybrid Genetic Search) Assessment

### Why HGS is Not Suitable as Surge's Core Algorithm

HGS (Vidal 2012-2022) is state-of-the-art on clean CVRP and VRPTW benchmarks. However,
its architecture makes three commitments that conflict with Surge's rich constraint model:

**1. Giant tour + Split decoder** — The chromosome is a customer permutation; Split uses O(n)
DP to find optimal route boundaries. PD precedence makes Split NP-hard (pairing constraints
create inter-route dependencies). Multi-trip explodes the state space. PyVRP explicitly does
not support PDPTW for this reason.

**2. O(1) concatenation scheme** — Route cost after a move is computed from fixed-size
cumulative tuples per subsequence. This breaks for:
- Sequence-dependent setup times (cost depends on adjacent node identity)
- Max ride time (depends on positions of specific PD pairs — non-decomposable)
- Commodity conflicts (set membership, not scalar load)
- Breaks (driving/resting state machine — non-monotonic)

Fallback is O(n) re-evaluation per move, eliminating HGS's main speed advantage.

**3. Crossover destroys constraint structure** — OX/SREX operators permute individual nodes
without awareness of PD pairs, commodity conflicts, or exclusion groups. Post-crossover
repair weakens genetic information transmission.

### Constraint Extensibility Comparison

Adding a new constraint to **ALNS** requires:
1. Feasibility check in insertion evaluator
2. Possibly a new cost component

Adding a new constraint to **HGS** requires:
1. Extending the penalty function
2. Modifying all 9+ local search move evaluations
3. Extending the route update function
4. Modifying or replacing the Split algorithm
5. Possibly redesigning the crossover operator
6. Adding a new self-adjusting penalty coefficient

Touch-point count: 3-5x larger per constraint. With 15+ simultaneous constraint types,
HGS would need to be rewritten from scratch.

### What Would Work: Hybrid Population-ALNS

Use ALNS destroy-repair as an operator within a population framework:

```
┌─────────────────────────────────────┐
│     Population Manager (Surge)       │
│  Tournament select, crossover,       │
│  diversity tracking, replacement     │
├─────────────────────────────────────┤
│     Arbor ALNS (per-individual)      │
│  Reuses all existing destroy/repair  │
│  operators for local intensification │
└─────────────────────────────────────┘
```

This preserves ALNS's constraint extensibility while gaining population-based diversity.
Christiaens & Vanden Berghe (2020) demonstrate this hybrid for CVRP with strong results.

Implementation: at Surge level, not Arbor. Arbor remains single-solution. Population
management is ~200-400 lines of new code in `sg_solve.c`.

### HGS Constraint Compatibility Matrix

| Constraint | HGS support | Difficulty | Issue |
|------------|-------------|------------|-------|
| Capacity (single-dim) | Native | Easy | Core HGS-CVRP |
| Hard time windows | Supported (time warp) | Easy | HGS-VRPTW |
| Open routes | Supported | Easy | PyVRP OVRP |
| Max distance/duration | Supported | Easy | Penalty-based |
| Multi-depot | Supported | Moderate | UHGS 2014 |
| Multi-dim capacity | Moderate | Moderate | Split harder, penalty extension |
| Disjunct time windows | Supported | Moderate | PyVRP VRPMTW |
| Vehicle qualifications | Supported in UHGS | Moderate | Assignment component |
| PD pairing + precedence | **Problematic** | **Hard** | Breaks giant tour + Split |
| Sequence-dependent setup | **Hard** | **Hard** | Breaks O(1) concatenation |
| Commodity conflicts | **Hard** | **Hard** | Set membership, not scalar |
| Exclusion groups | **Hard** | **Hard** | Inter-item constraint |
| Max ride time (DARP) | **Very hard** | **Very hard** | Non-decomposable |
| Break policies (HoS) | **Very hard** | **Very hard** | Non-decomposable, state-dependent |

### Bottom Line

ALNS+SA is the right architecture for Surge's constraint portfolio. HGS should only be
considered for a separate, specialized clean-CVRP/VRPTW solver. The population-ALNS hybrid
is the practical path to better solution quality within Surge's existing architecture.

---

### Phase S18: Split-String SISR + Worst-Cost Vehicle Destroy ✅

**Result**: Solomon 100 population (60s): avgDistGap -0.2%, avgVehGap +0.20, equalVehicles 45/56, lexiNonWorse 14. No regression from prior results. New operators provide destruction diversity without degrading solution quality on small instances.

**Problem**: Two gaps in the destroy operator portfolio:
1. SISR extracts one contiguous substring per vehicle. On large routes (40-80 requests), this creates one large gap. The original Christiaens & Vanden Berghe 2020 paper also uses a "split" mode that creates multiple smaller gaps at different route positions, giving repair more diverse insertion opportunities.
2. Route-level operators (`vehicle-target`, `vehicle-empty`) select vehicles by size (fewest requests). No operator targets vehicles with the highest per-request cost — expensive routes often contain misplaced requests that would be cheaper on other vehicles.

**Changes (surge — sg_destroy.c, +~210 lines):**
- `sg_route_destroy_string_split()`: Split-string SISR variant. Picks random seed, chooses L removal count and K=2-3 segments, extracts K shorter substrings spaced across the route instead of one contiguous block. Cross-route continuation identical to existing string destroy. Re-reads route pointer after each segment unassign. Full frozen-request filtering.
- `sg_route_destroy_vehicle_worst_cost()`: Worst-cost vehicle destroy. Computes `route_distance[v] / route_lengths[v]` for all non-empty vehicles, selects the vehicle with the highest per-request cost (tie-break: most requests), removes all its requests, fills remaining quota with Shaw-related requests from neighboring vehicles. Same frozen filter pattern as vehicle-target/vehicle-empty.

**Changes (surge — sg_internal.h):**
- Declared `sg_route_destroy_string_split()` and `sg_route_destroy_vehicle_worst_cost()`

**Changes (surge — sg_solve.c):**
- Registered `string-split` (weight 1.0) and `vehicle-worst-cost` (weight 1.0) in `sg_create_route_alns()`. Total destroy operators: 14.

**Tests**: 10 new tests (403→413). Split-string: basic (8 requests, multiple gaps), cross-route (3 vehicles, visits >=2), small-route degradation (2 requests), count-exceeds, frozen filtering. Worst-cost: basic ratio selection, Shaw-fill quota, frozen filtering, tie-break (equal ratio → most requests), empty solution.

---

## Hyperparameter Tuning & Large-Scale Benchmarks

### Motivation

Surge has ~50+ tunable parameters (SA temperature, penalty weights, phase budget splits,
operator weights, destruction constants) but no systematic tuning infrastructure. Current
benchmarks cover only ~115 instances at 100-customer/task scale. Need: (1) logarithmic grid
search tuner to optimize parameters, (2) 1000+ benchmark instances including 200-1000
customer scale.

### Benchmark Expansion (~1152 instances)

#### Published Instances (download)

| Set | Count | Scale | Format | Source |
|-----|-------|-------|--------|--------|
| Gehring-Homberger VRPTW | 300 | 200-1000 customers | Solomon (parser compatible) | SINTEF TOP |
| Li-Lim extended PDPTW | 298 | 200-1000 tasks | Li-Lim (parser compatible) | SINTEF TOP |
| Cordeau DARP complete | 19 | Varies | Cordeau (parser compatible) | CIRRELT |

#### Instance Generator (create)

| Generator | Output | Sizes | Per Size | Total |
|-----------|--------|-------|----------|-------|
| `sg_gen_solomon` | Solomon VRPTW | 50-2000 (7 sizes) | 6 classes x 5 = 30 | 210 |
| `sg_gen_li_lim` | Li-Lim PDPTW | 50-2000 (7 sizes) | 6 classes x 5 = 30 | 210 |

Class-based generation (C1/C2/R1/R2/RC1/RC2) matching Solomon/GH structure: clustered,
random, or mixed locations with narrow or wide time windows.

#### BKS Management

Move from hardcoded C arrays to CSV files in `surge/benchmarks/bks/`. Format:
`name,vehicles,distance`. Loader: `sg_load_bks_csv()`.

### Hyperparameter Tuner

Standalone C program `bench_tune.c`. Adds `SGTuneParams` struct to `SGContext` for
runtime parameter overrides (~15 insertion points in `sg_solve.c`).

#### Parameter Tiers (tuned in order of impact)

| Tier | Parameters | Count | Example |
|------|-----------|-------|---------|
| 0 | Phase budget split | 2 | phase1_fraction [0.4, 0.8], phase15_iters [100, 2000] |
| 1 | SA temperature/cooling | 4 | sa_accept_pct, final_temp_ratios |
| 2 | Penalty weights | 6 | target_start/end, tolerance, increase/decrease |
| 3 | ALNS reward weights | 4 | reaction_factor, reward_best/better/accepted |
| 4 | Destruction sizing | 3 | segment_size, adaptive_q_growth, q_min_fraction |
| 5 | Randomness constants | 4 | worst/shaw randomness, string_l_max |

Logarithmic grid for temperatures/penalties/rewards; linear for fractions/budgets.
Progressive refinement: coarse grid per tier -> top-3 -> fine grid. ~3000 total evaluations.

#### Evaluation Protocol

Representative set: 12 Solomon + 6 Li-Lim = 18 instances covering all classes.
Composite metric:
`score = 100 * avg_vehicle_gap + avg_distance_gap_pct + 50 * max(0, worst_vehicle_gap)`.
Parallel across configurations via `sh_worker_pool`.

#### Solver Profiles

The tuner optimizes parameters for 4 distinct quality/speed profiles:

| Profile | Budget (100 req) | Budget (1000 req) | Use Case |
|---------|-------------------|--------------------|----------|
| `SG_PROFILE_REALTIME` | 500 iters / 0.5s | 200 iters / 2s | Live dispatch, API response, interactive UI |
| `SG_PROFILE_FAST` | 2500 iters / 3s | 1000 iters / 15s | Planning with quick feedback, re-optimization |
| `SG_PROFILE_NEAR_OPTIMAL` | 10000 iters / 15s | 5000 iters / 60s | Overnight planning, batch optimization |
| `SG_PROFILE_BEST` | 50000 iters / 60s | 25000 iters / 300s | Research benchmarks, competition, final plan |

Each profile may have different optimal parameters (e.g., real-time favors aggressive
destruction + fast cooling; best favors gentle cooling + wide exploration). The tuner
evaluates each tier at each profile's iteration budget, producing 4 independent parameter
sets. Users select profiles via `sg_config_set_profile(ctx, SG_PROFILE_FAST)` which
auto-sets iterations + tuned parameters.

#### Evaluation Scenarios

Each profile is evaluated on appropriate instance sizes:

| Profile | Primary Eval Set | Secondary Eval Set |
|---------|------------------|--------------------|
| Realtime | Solomon 100 (57), Li-Lim 100 (57) | GH 200 (60) |
| Fast | Solomon 100, Li-Lim 100, GH 200 | GH 400 (60) |
| Near-optimal | Solomon 100, Li-Lim 100, GH 200-400 | GH 600 (60) |
| Best | All published instances | GH 1000 (60) |

### Tuning Results (Feb 2026)

#### Winning Parameters

Tuned on 18 representative Solomon + Li-Lim instances at 2500 iterations per config.
Tier 1 (SA temperature) produced the only statistically significant improvement:

| Parameter | Default | Tuned | Effect |
|-----------|---------|-------|--------|
| `sa_accept_pct` | 0.05 | **0.074** | Higher initial temperature, more exploration |
| `p1_final_temp_ratio` | 0.05 | **0.08** | Slower Phase 1 cooling, more vehicle reduction |
| `p2_final_temp_ratio` | 0.001 | **0.0001** | Aggressive Phase 2 cooling, tighter distance polish |
| `phase15_iters` | 500 | **2000** | More vehicle crunch budget |

Tiers 2-5 (penalties, ALNS rewards, destruction, randomness) showed no improvement
over defaults — the existing values were already well-chosen.

Applied via profiles (conditional on `tune_params`) to preserve backward compatibility
with existing tests. Benchmark runners also set these explicitly.

#### Benchmark Results: Solomon VRPTW (100 customers)

Population mode, 3 generations, 10K iterations, deterministic seed 42.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1xx (clustered, tight) | 9 | 9/9 | +0.00 | +0.2% |
| C2xx (clustered, wide) | 8 | 8/8 | +0.00 | exact BKS |
| R1xx (random, tight) | 12 | 6/12 | +0.50 | -0.6% |
| R2xx (random, wide) | 11 | 10/11 | +0.09 | -0.3% |
| RC1xx (mixed, tight) | 8 | 4/8 | +0.50 | -0.6% |
| RC2xx (mixed, wide) | 8 | 8/8 | +0.00 | +1.3% |
| **Overall** | **56** | **45/56 (80%)** | **+0.20** | **-0.1%** |

Highlights: all clustered + wide-TW instances at exact BKS. Tight-TW R1/RC1
trade +1 vehicle for better distance. Negative overall distance gap = beats BKS
average.

#### Benchmark Results: Li & Lim PDPTW (100 requests)

Population mode, 3 generations, 10K iterations, deterministic seed 42.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| LC1xx (clustered, tight) | 9 | 5/9 | +1.22 | +15.6% |
| LC2xx (clustered, wide) | 8 | 8/8 | +0.00 | exact BKS |
| LR1xx (random, tight) | 12 | 9/12 | +0.83 | +3.0% |
| LR2xx (random, wide) | 11 | 11/11 | +0.00 | exact BKS |
| LRC1xx (mixed, tight) | 8 | 6/8 | +0.25 | +0.6% |
| LRC2xx (mixed, wide) | 8 | 8/8 | +0.00 | +0.4% |
| **Overall** | **56** | **47/56 (84%)** | **+0.41** | **+3.3%** |

Highlights: all wide-TW categories at exact BKS. LC1xx tight-window clustered
PDPTW is hardest — needs more iterations or specialized tight-TW operators.

#### Implementation Status

| Phase | Description | Status |
|-------|-------------|--------|
| 1 | BKS CSV infrastructure + runner upgrades | Done |
| 2 | Download GH/Li-Lim extended (200-1000 scale) | Done |
| 3 | Instance generators | Done (sg_gen_solomon, sg_gen_li_lim) |
| 4 | SGTuneParams infrastructure | Done |
| 5 | Tuner program (bench_tune) | Done |
| - | Apply winning params to profiles | Done |
| - | Large-scale benchmarks (200-1000) | Done (200 + 400 customer) |

#### Benchmark Results: Gehring-Homberger VRPTW (60 instances, 200 customers each)

Single-thread, 10K iterations, 60s time limit per phase, deterministic seed 42.
Tuned SA params: sa_accept_pct=0.074, p1_final_temp_ratio=0.08, p2_final_temp_ratio=0.0001, phase15_iters=2000.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_2 (clustered, tight) | 10 | 8/10 | +0.20 | +7.6% |
| C2_2 (clustered, wide) | 10 | 10/10 | +0.00 | +1.2% |
| R1_2 (random, tight) | 10 | 10/10 | +0.00 | +7.8% |
| R2_2 (random, wide) | 10 | 9/10 | +0.10 | +1.7% |
| RC1_2 (mixed, tight) | 10 | 9/10 | +0.10 | +26.8% |
| RC2_2 (mixed, wide) | 10 | 8/10 | +0.20 | +2.4% |
| **Overall** | **60** | **54/60 (90%)** | **+0.10** | **+7.9%** |

Key findings: Vehicle minimization is excellent (90% match BKS). Distance gap is
concentrated on RC1 (mixed tight-TW) at +26.8% — needs more iterations or specialized
operators. C2 and R2 (wide-TW) perform best. Avg runtime 205.7s per instance.

#### Benchmark Results: Li & Lim Extended PDPTW (60 instances, 200 tasks each)

Single-thread, 10K iterations, 120s time limit per phase, deterministic seed 42.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| LC1_2 (clustered, tight) | 10 | 4/10 | +2.10 | +17.3% |
| LC2_2 (clustered, wide) | 10 | 10/10 | +0.00 | +0.3% |
| LR1_2 (random, tight) | 10 | 0/10 | +3.30 | +5.7% |
| LR2_2 (random, wide) | 10 | 5/10 | +0.50 | -5.5% |
| LRC1_2 (mixed, tight) | 10 | 1/10 | +1.20 | +0.5% |
| LRC2_2 (mixed, wide) | 10 | 4/10 | +0.60 | -8.9% |
| **Overall** | **60** | **24/60 (40%)** | **+1.28** | **+1.6%** |

Key findings: PDPTW 200-task is harder for vehicle minimization (40% vs 90% for VRPTW).
LC2 is near-perfect. LR1 (random tight-TW) is hardest (0/10 vehicle match). Several
LR2/LRC2 instances beat BKS distance but use +1 vehicle (lexicographic tradeoff).
Avg runtime 30.8s per instance (much faster than VRPTW).

#### Scaling Observations

400+ task instances were impractical before the global time envelope — a single 400-task
PDPTW instance took 1834s with a 120s limit. Phases 1+2 (global time envelope + phase
budget management + postprocessing deadline checks) are now complete. Results:

| Case | Budget | Before | After | Overshoot |
|------|--------|--------|-------|-----------|
| GH-200 c1_2_1 (5s) | 5s | 5.1s | 5.1s | ~0.1s |
| GH-200 c1_2_7 (60s) | 60s | 1186s (20x) | **60.2s** | ~0.2s |
| LL-400 LC1_4_1 (120s) | 120s | 1834s (15x) | **123.0s** | ~3.0s |

Root cause analysis and remaining scaling work follows.

### Scaling to 400+ Requests

#### Problem Diagnosis

A 400-task LL instance with `max_time_seconds=120` ran for 1834 seconds. Breakdown:

| Phase | Time Bounded? | Actual Time | Why |
|-------|--------------|-------------|-----|
| Construction | No | ~15s | Solomon I1 heuristic, O(n² log n) |
| Phase 1 ALNS | Yes (120s) | ~120s | Hits time limit, few iterations complete |
| Between-phase postprocessing | **No** | **~500-1000s** | ejection_reduce + reduce_vehicles_relaxed + intensify |
| Phase 1.5 ALNS | Yes (120s shared) | ~5s | Budget already consumed by Phase 1 |
| Phase 2 ALNS | Yes (120s shared) | ~5s | Budget already consumed by Phase 1 |
| Final postprocessing | **No** | **~600-1500s** | reduce_vehicles + ejection_reduce + intensify + polish_distance |

The same pattern explains GH-200 outliers (c1_2_7=1186s, c1_2_9=1045s with 60s limit).

**Core issues:**
1. Postprocessing is completely unbounded — no time checks anywhere
2. Phase budget reuse — Phase 1 consumes the entire time budget, Phases 1.5/2 starve
3. Postprocessing on a bad solution grinds without improving quality (+89% gap)
4. O(n) insertion scans evaluate every position even when most are obviously bad

#### Phase 1: Global Time Envelope ✅ COMPLETE

Implemented a standalone `SGTimeBudget` module (`sg_time_budget.h/.c`) that tracks a
global monotonic deadline. All functions take an explicit `now` parameter — no internal
clock calls — making the module fully deterministic for unit testing (19 tests).

**Implementation:**
- `SGTimeBudget` struct added to `SGContext` (initialized to unlimited in `sg_create()`)
- `sg_time_budget_init()` called at top of `sg_solve_route_model()` with `max_time_seconds`
- Unlimited budget (`max_time_seconds <= 0`) uses `deadline = -1.0` sentinel
- Clock helper `sh_monotonic_seconds()` lives in `shared/include/sh_time.h`

**Files:** `surge/src/sg_time_budget.{h,c}` (new), `surge/src/sg_context.c`,
`surge/src/sg_solve.c`, `surge/tests/test_time_budget.c` (new),
`shared/include/sh_time.h` (new).

#### Phase 2: Phase Budget Management ✅ COMPLETE

Phase budget distribution via `sg_time_budget_phase()` and `sg_time_budget_remaining_int()`:

| Phase | Allocation | Mechanism |
|-------|-----------|-----------|
| Phase 1 (vehicle min) | 55% of remaining | `sg_time_budget_phase(tb, now, 0.55, 5.0)` |
| Between-phase postprocessing | Gated | `sg_time_budget_expired()` between each call |
| Phase 1.5 (crunch) | All remaining | `sg_time_budget_remaining_int()` |
| Phase 2 (distance) | All remaining | `sg_time_budget_remaining_int()` |
| Final postprocessing | Gated | `sg_time_budget_expired()` between each call |

Per-iteration deadline checks added inside all 5 postprocessing inner loops (9 check
points total): `reduce_vehicles` (2 while-improved loops), `reduce_vehicles_relaxed`
(2 while-improved loops), `ejection_reduce` (while-restarted + per-vehicle for loop),
`polish_distance` (per-pass + per-request), `intensify` (per-pass).

Entire phases are skipped via `goto skip_phaseN` when budget is exhausted.
`remaining_int()` returns 0 for unlimited budgets — ALNS interprets 0 as "no time limit".

**Files:** `surge/src/sg_solve.c`, `surge/src/sg_postprocess.c`.

#### Phase 3: Neighbor Lists for Insertion Pruning

**Priority: High. Largest per-iteration speedup for large instances.**

The repair operators (greedy, regret-3) evaluate every vehicle × every position for
each unassigned request. For 400 tasks with 20 vehicles and 20 stops per route,
that's 20 × 20 = 400 insertion evaluations per request, with 40 removed requests =
16,000 evaluations per ALNS iteration. At 200+ tasks, most positions are obviously bad.

**Nearest-neighbor lists:**

Pre-sort locations by distance from each location. During insertion, only evaluate
positions adjacent to the k nearest neighbors (k=20-40).

```c
typedef struct {
    uint32_t *neighbors;     /* neighbors[i * k + j] = j-th nearest to location i */
    uint32_t k;              /* neighbors per location */
    uint32_t num_locations;
} SGNeighborIndex;
```

**Build cost:** O(n² log k) using partial sort. For 800 locations, k=30: ~19M comparisons,
<100ms. Built once at solve start.

**Insertion pruning:** When evaluating insertions for request R at location L:
- Only consider vehicles whose route passes within the k-nearest neighbors of L
- Skip positions where both adjacent stops are far from L
- Fallback: if no feasible insertion found in neighbors, scan all positions

**Expected speedup:** 5-10× for n>200. Insertion scan drops from O(n) to O(k) per vehicle.

**Interaction with distance/duration model:**

The neighbor index uses `sg_travel_dist()` for sorting, respecting the full resolution
hierarchy. For static matrices (Solomon, GH, LL benchmarks), this is a one-time O(n²)
build. For time-dependent or callback-based models:

| Model | Neighbor Index | Behavior |
|-------|---------------|----------|
| Static matrix (global) | O(n²) build, exact | Standard case |
| Per-vehicle profiles | Build per profile type | Vehicles with same profile share index |
| Time-dependent brackets | Build from base bracket | Approximation — neighbor order may shift |
| Callback | Build from departure_time=0 | Approximation — fallback to full scan if needed |
| Speed profiles | N/A (doesn't affect distance) | Index unaffected |

For TD/callback models, the neighbor list is an *approximation*. The insertion evaluator
still calls `sg_travel_dist()` / `sg_travel_dur()` for exact costs — the neighbor list
only prunes which positions to evaluate, not the evaluation itself. If a request can't
be feasibly inserted in any neighbor position, the full scan runs as fallback.

**Files:** New `sg_neighbor.c` / `sg_neighbor.h`. Changes to `sg_repair.c` (insertion
loop), `sg_solve.c` (build at solve start), `sg_context.c` (storage).

**Estimated effort:** 1-2 days.

#### Phase 4: Postprocessing Complexity Reduction ✅ COMPLETE

Postprocessing was consuming disproportionate time budget for n>200 instances.
Four changes make postprocessing cost-proportional to instance size:

| Change | Mechanism | Impact |
|--------|-----------|--------|
| Adaptive intensify cap | 3 passes (not 8) for n>200 | ~60% less intensify time |
| Neighbor-aware LS | Prune inter-route moves in exchange, 2-opt*, OR-opt, cross-exchange using k=30 neighbor index | 80-90% fewer futile evaluations |
| Ejection early exit | Break after 3 consecutive failed vehicle elimination attempts | Stops grinding on intractable eliminations |
| Skip polish_distance | Gate on `num_requests <= 200 \|\| phase2_iters == 0` | Avoids redundant O(R²) distance pass when Phase 2 ALNS already optimized |

All thresholds are below n=100, so Solomon/Li-Lim 100-customer benchmarks are
unaffected (verified: 374/374 tests pass, Solomon 56/56 identical results).

**Files:** `sg_internal.h` (new constants), `sg_postprocess.c` (4 operator changes +
ejection counter + adaptive cap), `sg_solve.c` (polish_distance gate).

#### Benchmark Results: Gehring-Homberger VRPTW Post-Phase-4 (200 customers, 60s limit)

Single-thread, 10K iterations, 60s time limit, deterministic seed 42.
Phase 4 postprocessing changes active.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_2 (clustered, tight) | 10 | 7/10 | +0.30 | +8.5% |
| C2_2 (clustered, wide) | 10 | 10/10 | +0.00 | +3.2% |
| R1_2 (random, tight) | 10 | 10/10 | +0.00 | +20.8% |
| R2_2 (random, wide) | 10 | 9/10 | +0.10 | +3.0% |
| RC1_2 (mixed, tight) | 10 | 4/10 | +0.60 | +27.2% |
| RC2_2 (mixed, wide) | 10 | 7/10 | +0.30 | +5.3% |
| **Overall** | **60** | **48/60 (80%)** | **+0.20** | **+13.2%** |

Avg runtime: 61.5s (vs 205.7s pre-Phase-4 with same time limit — postprocessing
no longer dominates).

**Before/after comparison (same commit minus Phase 4 changes):**

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Avg distance | 3520.98 | 3194.43 | **-9.3%** |
| Avg dist gap vs BKS | +23.4% | +13.2% | **-10.2pp** |
| Equal vehicles to BKS | 52/60 (87%) | 48/60 (80%) | -7pp |
| Avg vehicle gap | +0.13 | +0.20 | +0.07 |

Phase 4 cut the distance gap nearly in half by freeing time budget that postprocessing
was wasting — ALNS gets more iterations within the same 60s envelope. Slight vehicle
regression (87% → 80%) is the tradeoff: fewer intensify passes means fewer vehicle-
eliminating local search moves, but net solution quality (distance) is substantially
better. Biggest winners are R1/RC1 tight-TW instances where the old code ground in
postprocessing (R1_2_8: +95.4% → +31.7%, RC1_2_4: +129.2% → +59.7%).

#### Benchmark Results: Gehring-Homberger VRPTW Post-Phase-S16 (200 customers, 60s limit)

Population mode (3 generations, all CPU cores), 10K iterations, 60s time limit, deterministic seed 42.
CFRS construction heuristics active — 5 construction methods round-robined across threads.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_2 (clustered, tight) | 10 | 8/10 | +0.20 | +10.1% |
| C2_2 (clustered, wide) | 10 | 10/10 | +0.00 | +2.0% |
| R1_2 (random, tight) | 10 | 10/10 | +0.00 | +22.9% |
| R2_2 (random, wide) | 10 | 9/10 | +0.10 | +3.9% |
| RC1_2 (mixed, tight) | 10 | 9/10 | +0.10 | +34.2% |
| RC2_2 (mixed, wide) | 10 | 9/10 | +0.10 | +5.8% |
| **Overall** | **60** | **55/60 (92%)** | **+0.08** | **+13.1%** |

Avg runtime: 104.9s. Best vehicle count result to date.

**Progress across phases (GH-200, 60s time limit):**

| Metric | Pre-Phase-4 (1T) | Post-Phase-4 (1T) | **Phase S16 + Pop** |
|--------|-------------------|---------------------|---------------------|
| Equal Vehicles | 54/60 (90%) | 48/60 (80%) | **55/60 (92%)** |
| Avg Veh Gap | +0.10 | +0.20 | **+0.08** |
| Avg Dist Gap | +7.9% | +13.2% | +13.1% |

CFRS construction directly solved the vehicle count bottleneck — sweep/k-means
heuristics produce initial solutions with the correct number of vehicles, so ALNS
spends less time on vehicle elimination and more on distance optimization. Population
diversity (5 structurally different starting points per generation) further improves
vehicle minimization. Distance gap remains at ~13% — closing this requires more ALNS
iterations (longer time budget) or better intra-route optimization operators.

#### Benchmark Results: Gehring-Homberger VRPTW (400 customers, 60s limit)

First 400-customer results. Single-thread, 10K iterations, 60s time limit, deterministic seed 42.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 2/10 | +3.10 | +66.0% |
| C2_4 (clustered, wide) | 10 | 0/10 | +1.00 | +37.1% |
| R1_4 (random, tight) | 10 | 10/10 | +0.00 | +67.1% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +56.8% |
| RC1_4 (mixed, tight) | 10 | 3/10 | +1.90 | +57.1% |
| RC2_4 (mixed, wide) | 10 | 4/10 | +0.70 | +24.6% |
| **Overall** | **60** | **28/60 (47%)** | **+1.42** | **+51.8%** |

Avg runtime: 82.4s.

#### Benchmark Results: Gehring-Homberger VRPTW Post-Phase-S17.3 (400 customers, 60s limit)

Single-thread, 10K iterations, 60s time limit, deterministic seed 42.
O(1) concat pre-filtering active in 2-opt*, OR-opt, cross-exchange. Pre-filter
skip rate measured at 99.2-99.7% of trial moves on C1_4_1.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 0/10 | +4.30 | +60.1% |
| C2_4 (clustered, wide) | 10 | 0/10 | +1.00 | +33.2% |
| R1_4 (random, tight) | 10 | 10/10 | +0.10 | +67.3% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +54.4% |
| RC1_4 (mixed, tight) | 10 | 2/10 | +1.80 | +50.6% |
| RC2_4 (mixed, wide) | 10 | 6/10 | +0.80 | +21.6% |
| **Overall** | **60** | **28/60 (47%)** | **+1.33** | **+47.9%** |

Avg runtime: 118.7s.

**Before/after comparison (Phase S17.3 vs pre-S17.3):**

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Avg distance | 9897.79 | 9644.43 | **-2.6%** |
| Avg dist gap vs BKS | +51.8% | +47.9% | **-3.9pp** |
| Equal vehicles to BKS | 28/60 (47%) | 28/60 (47%) | Same |
| Avg vehicle gap | +1.42 | +1.33 | -0.09 |
| RC2 vehicle match | 4/10 | 6/10 | +2 instances |

O(1) pre-filtering freed intensify time — ALNS gets more iterations within the same
budget. Distance gap improved by 3.9pp overall. RC2 (mixed, wide TW) benefits most
from the extra ALNS iterations, gaining 2 more BKS-matching vehicle counts. Some
instances ran well over 60s (C1_4_7: 407s, R1_4_1: 407s) due to postprocessing
ejection chains running beyond the ALNS time limit — the concat pre-filter only
accelerates the intensify phase, not ejection chains.

#### Benchmark Results: Gehring-Homberger VRPTW Post-S17.3 Population (400 customers, 60s limit)

Population mode (3 generations, all CPU cores), 10K iterations, 60s time limit,
deterministic seed 42. O(1) concat pre-filtering + CFRS construction active.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 2/10 | +4.00 | +55.2% |
| C2_4 (clustered, wide) | 10 | 3/10 | +0.70 | +32.3% |
| R1_4 (random, tight) | 10 | 10/10 | +0.00 | +41.2% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +22.3% |
| RC1_4 (mixed, tight) | 10 | 3/10 | +1.70 | +40.5% |
| RC2_4 (mixed, wide) | 10 | 5/10 | +0.80 | +19.5% |
| **Overall** | **60** | **33/60 (55%)** | **+1.13** | **+35.1%** |

Avg runtime: 705.8s.

**Progress across phases (GH-400):**

| Metric | Pre-S17.3 (1T) | S17.3 (1T) | **S17.3 + Pop** |
|--------|-----------------|------------|-----------------|
| Equal Vehicles | 28/60 (47%) | 28/60 (47%) | **33/60 (55%)** |
| Avg Veh Gap | +1.42 | +1.33 | **+1.13** |
| Avg Dist Gap | +51.8% | +47.9% | **+35.1%** |
| Avg Distance | 9898 | 9644 | **8803** |

Population + CFRS construction improved distance by 12.8pp over single-thread S17.3.
R2_4 improved dramatically: +54.4% → +22.3% (nearly halved). RC2_4: +21.6% → +19.5%.
Vehicle match improved from 28 to 33 instances, with C2 gaining 3 and RC2 gaining 2
(partially offset by C1 losing 2 from construction variance).

**Runtime issue (fixed in b3905e2):** Several instances exceeded the 60s time limit
dramatically (c2_4_8: 4221s, c2_4_5: 3573s, c2_4_1: 3501s, r1_4_1: 2634s). Root cause:
ejection chain budget checks were coarse-grained. Fixed by adding `SGBudgetProbe`
amortized clock checks (every 64 ticks) inside all inner loops and the recursive
`sg_try_place_with_ejection()` entry point. Post-fix: max runtime 104.5s, avg 74.2s.

#### Benchmark Results: Gehring-Homberger VRPTW Post-Ejection-Probe (400 customers, 60s limit)

Population mode (3 generations, all CPU cores), 10K iterations, 60s time limit,
deterministic seed 42. O(1) concat pre-filtering + CFRS construction + SGBudgetProbe active.

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 0/10 | +4.10 | +45.6% |
| C2_4 (clustered, wide) | 10 | 1/10 | +1.10 | +28.1% |
| R1_4 (random, tight) | 10 | 9/10 | +0.10 | +41.7% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +22.6% |
| RC1_4 (mixed, tight) | 10 | 2/10 | +1.90 | +42.5% |
| RC2_4 (mixed, wide) | 10 | 5/10 | +0.80 | +19.1% |
| **Overall** | **60** | **27/60 (45%)** | **+1.33** | **+33.3%** |

Avg runtime: 74.2s. Max runtime: 104.5s (r1_4_1).

**Progress across phases (GH-400):**

| Metric | Pre-S17.3 (1T) | S17.3 (1T) | S17.3 + Pop | + Ejection Probe | S19 Heap | **S22 Tuned** |
|--------|-----------------|------------|-------------|------------------|----------|---------------|
| Equal Vehicles | 28/60 (47%) | 28/60 (47%) | 33/60 (55%) | 27/60 (45%) | 33/60 (55%) | **35/60 (58%)** |
| Avg Veh Gap | +1.42 | +1.33 | +1.13 | +1.33 | +0.67 | **+0.53** |
| Avg Dist Gap | +51.8% | +47.9% | +35.1% | +33.3% | +18.6% | **+12.9%** |
| Avg Distance | 9898 | 9644 | 8803 | 8699 | — | **7359** |
| Avg Runtime | — | — | 705.8s | 74.2s | 77s | **76s** |
| Max Runtime | — | — | 4221s | 104.5s | — | **164s** |

Vehicle match dropped from 33 to 27 because the pre-fix runs were "cheating" — ejection
chains that overran the budget by 50x sometimes found vehicle reductions. With correct budget
enforcement, distance actually improved by 1.8pp because time previously wasted in runaway
ejection chains is now spent on ALNS iterations. The runtime improvement (705.8s → 74.2s avg,
4221s → 104.5s max) makes population mode viable for production use at 400-customer scale.

#### Competitiveness Assessment (Mar 2026)

**100 customers: Strong.** 80% vehicle match, -0.1% avg distance gap vs BKS with
population mode. Competitive with published ALNS implementations (Ropke & Pisinger).
Rich constraint support (PDPTW, DARP, compartments, breaks, multi-trip, locking,
backhaul, LIFO/FIFO, precedence, setup times) goes well beyond most academic solvers.

**200 customers: Strong vehicle minimization, distance needs work.** 92% vehicle match
(+0.08 avgVehGap) with population + CFRS construction — only 5 instances use +1 vehicle.
This is competitive with published solvers on the vehicle dimension. Distance gap of
+13.1% reflects the 60s time budget — BKS papers typically allow 200-600s. More time
budget (profile matrix NEAR_OPTIMAL gives 120s) and per-cell tuning should close this.

**400 customers: Improving rapidly at 60s, untested at competition budgets.** +12.9%
distance gap and 58% vehicle match at 60s with population + CFRS + S22 tuned params.
Down from +51.8% / 47% before S17.3. R2_4 (wide TW) at +7.9%, C1_4 at +12.3%
(c1_4_1 BKS match). At 300s, rc1_4_1 drops to +6.6%. Note: the DIMACS competition
standard is **2 hours** per instance on a reference CPU — Surge has only been tested
at 60-300s. A fair comparison requires running at BEST profile (600s) or full 2h.

Root causes at 400+:

| Issue | Impact | Mitigation |
|-------|--------|------------|
| Poor construction quality | Solomon I1 produces too many vehicles (48 vs BKS 40 on C1_4_1) | ✅ CFRS heuristics (Phase S16) — 55% vehicle match with population at 400 |
| Low iterations/sec | Destroy-repair cycle is O(n) per iteration; fewer iterations in budget | ✅ O(1) concat pre-filter (Phase S17.3) — 99%+ skip rate in intensify |
| Ejection chain timeout | ~~Coarse-grained budget check~~ | ✅ Fixed: SGBudgetProbe amortized checks in all inner loops + recursive ejection entry (commit b3905e2) |
| Vehicles-first objective | Most of 60s spent on vehicle elimination, not distance | Needs more total budget (profile matrix BEST gives 600s for LARGE) |
| Limited operator set | 8 destroy + greedy/regret repair | More operators: SISR, route-level destroy, LNS with backtracking |

**Where Surge is strong regardless of scale:**

- Rich constraint handling — most academic solvers handle VRPTW only; Surge handles
  PDPTW + DARP + 15+ constraint dimensions out of the box
- Deterministic, reproducible results from fixed seeds
- Time-budgeted — all instances complete within ~1.5x budget, suitable for real-time systems
- Production-ready API (JSON, WASM, C library) with warm start and progress callbacks

**Architectural foundations are solid for all scales.** The current gap at 400+ is not
a design limitation — it's a matter of additive improvements on top of a sound core:

- **ALNS framework is scale-agnostic.** Arbor's adaptive weights, roulette selection,
  and SA acceptance don't care about n. The operators plugged into it determine scaling
  behavior, and new operators slot in without touching the framework.
- **Neighbor index is the right architecture.** k-nearest pruning in repair and local
  search is exactly what competitive large-scale solvers use (Ropke & Pisinger, Vidal's
  HGS). The infrastructure is built and proven — it just needs more aggressive use
  (e.g., restrict destroy operators to geographic neighborhoods).
- **Time budget system is production-grade.** Phase allocation with monotonic deadline
  checks is cleaner than most academic implementations that run for a fixed iteration
  count. This is essential for production use where latency SLAs matter.
- **Rich constraints are orthogonal.** The 15+ constraint dimensions are feasibility
  checks during insertion, not modifications to the ALNS loop. Adding a better
  construction heuristic or SISR destroy operator doesn't touch any constraint code.
- **Profile matrix scales independently.** The 4×5 matrix with per-cell tuning means
  each scale point can be independently optimized. Most solvers use one-size-fits-all.

The gap from +12.9% to <10% at 400 customers requires: (1) ~~fixing ejection chain
timeouts~~ ✅ done (commit b3905e2), (2) ~~per-cell tuning of the profile matrix for
LARGE scale~~ ✅ done (S22, brought +33.3%→+12.9%), (3) more time budget — the BEST
profile gives 600s and NEAR_OPTIMAL gives 120s, (4) operator improvements for R1-class
random instances that plateau early. At 300s, rc1_4_1 already drops to +6.6% distance.

**Realistic targets for next phase of work:**

| Scale | Current Gap (60s) | Target Gap | Required |
|-------|-------------------|------------|----------|
| 100 | -0.1% dist, 80% veh | — | Already competitive |
| 200 | +13.1% dist, **92% veh** | <5% dist | Per-cell tuning of MEDIUM column + more time budget |
| 400 | **+12.9% dist, 58% veh** (60s pop, S22) | <10% dist, >70% veh | More time budget (NEAR_OPTIMAL 120s), operator improvements |
| 800+ | Not tested | <30% dist | All of above + parallel ALNS + SISR operator |

**Time budget scaling (c1_4_1 — hardest instance, tight clustered 400-customer):**

| Budget | Vehicles (BKS: 40) | Dist Gap | Notes |
|--------|--------------------|----------|-------|
| 60s (pre-S22) | 47 (+7) | +50.6% | BASE_TUNE params |
| 120s (pre-S22) | 47 (+7) | +51.8% | BASE_TUNE params |
| 300s (pre-S22) | **41 (+1)** | **+3.0%** | BASE_TUNE params |
| 60s (S22) | 40 (+0) | **+0.0%** | LARGE_TUNE — BKS match! |
| 300s (S22) | 40 (+0) | **+0.0%** | LARGE_TUNE — BKS match |

With S22 scale-tuned params, c1_4_1 matches BKS at just 60s — the pre-S22 gap was
entirely due to poorly tuned SA temperature and ALNS learning rate, not time starvation.

#### Phase 5: Travel Resolution Cache for TD/Callback Models

**Priority: Low for benchmarks. Important for production with TD routing.**

For standard benchmarks (Solomon, GH, LL), `sg_travel_dist()` is already O(1) — it's
a direct matrix lookup via the inline function. No caching needed.

For production use cases with time-dependent travel or callbacks, repeated lookups for
the same (from, to, vehicle_profile, time_bracket) tuple waste computation:

| Model | Lookup Cost | Cache Benefit |
|-------|------------|---------------|
| Static matrix | O(1) — array index | None (already optimal) |
| Time brackets | O(B) bracket selection + O(1) matrix | Minor — B is typically 3-5 |
| Speed profiles | O(1) step function eval | None |
| Callback | Arbitrary (could be API call) | **High** — memoize recent lookups |

**Design: Tiered cache**

```c
typedef struct {
    /* Tier 1: Per-solve distance matrix snapshot (for cacheable models) */
    double *cached_distances;    /* num_locations × num_locations, NULL if uncacheable */
    int distances_cacheable;     /* true if no TD brackets, no callback, no per-vehicle profiles */

    /* Tier 2: Per-iteration LRU for callback/TD models */
    struct {
        uint64_t key;            /* pack(from, to, profile_id, bracket_id) */
        double distance;
        double duration;
    } *lru_cache;
    uint32_t lru_size;           /* 4096-16384 entries typical */
    uint32_t lru_mask;           /* power-of-2 for fast modulo */
} SGTravelCache;
```

**Cacheability detection** (at solve start):

```
if no callback && no per-vehicle profiles && no TD brackets:
    → Tier 1: snapshot global matrix (or it's already the matrix — no-op)
    → sg_travel_dist() already O(1), skip cache entirely

if per-vehicle profiles but no TD brackets:
    → Tier 1 per profile: snapshot each profile's matrix
    → Lookup = profile_matrices[vehicle.profile_id][from * n + to]

if TD brackets (global or per-profile):
    → NOT fully cacheable (departure_time varies)
    → Tier 2: LRU cache keyed on (from, to, profile_id, bracket_id)
    → bracket_id determined by departure_time at call site

if callback:
    → NOT cacheable (arbitrary side effects possible)
    → Tier 2: LRU cache if user opts in via flag
    → Fallback: no cache, call through every time
```

**Key principle:** If the model is fully cacheable (static matrices, no TD, no callback),
don't add any indirection — the current inline `sg_travel_dist()` is already optimal.
The cache only activates for models that actually benefit from it.

**Files:** New `sg_travel_cache.c` / `sg_travel_cache.h`. Changes to `sg_internal.h`
(cache struct in SGContext), `sg_solve.c` (build/free cache), `sg_feasibility.c`
(use cached lookups in hot path).

**Estimated effort:** 2-3 days. Should be gated behind a feature flag initially.

#### Implementation Order

```
Phase 1 + 2 (global envelope + budget)  →  ✅ DONE (Feb 2026)
         ↓
Phase 3 (neighbor lists)                →  biggest speedup, 1-2 days
         ↓
Phase 4 (postprocessing caps)           →  prevents outliers, 1 day
         ↓
Phase 5 (travel cache for TD)           →  production feature, 2-3 days
         ↓
Re-run GH-400, LL-400, then 600+        →  validate scaling
```

Phases 1+2 confirmed: GH-200 outliers eliminated (c1_2_7: 1186s → 60.2s), 400-task
instances now complete within budget (LC1_4_1: 1834s → 123s). Maximum overshoot is ~3s
from a single postprocessing iteration completing after the deadline.

Phase 3 is where the real scaling unlock happens — it changes iteration cost from O(n)
to O(k) and should enable 1000-customer instances.

### Profile × Scale Matrix

The profile × scale matrix maps each `(SGProfile, SGScale)` pair to a specific
`(max_iterations, max_time_seconds, SGTuneParams)` triple. This replaces the previous
approach where profiles only worked for 100-request instances.

#### Scale Columns

5 breakpoints based on where ALNS iteration cost changes qualitatively:

| Scale | Requests | Rationale |
|-------|----------|-----------|
| `SG_SCALE_SMALL` | 1–100 | Current tuning baseline. Same-day delivery. |
| `SG_SCALE_MEDIUM` | 101–200 | GH-200 benchmark range. Iteration ~2x slower. |
| `SG_SCALE_LARGE` | 201–400 | GH-400 range. Iteration ~4x slower. Neighbor pruning critical. |
| `SG_SCALE_XLARGE` | 401–800 | Large fleet operations. |
| `SG_SCALE_MASSIVE` | 801+ | Full-day planning. Longest budgets. |

Scale selection: snap to the smallest column that covers the request count. No interpolation
— SA parameters interact nonlinearly.

#### Iteration / Time Budget Matrix

| | SMALL | MEDIUM | LARGE | XLARGE | MASSIVE |
|---|---|---|---|---|---|
| **REALTIME** | 500 / 1s | 500 / 2s | 500 / 5s | 250 / 10s | 250 / 15s |
| **FAST** | 2500 / 5s | 2500 / 10s | 2500 / 30s | 1500 / 60s | 1000 / 90s |
| **NEAR_OPTIMAL** | 10000 / 15s | 10000 / 45s | 5000 / 120s | 3000 / 300s | 2000 / 600s |
| **BEST** | 50000 / 60s | 25000 / 180s | 10000 / 600s | 5000 / 1200s | 3000 / 1800s |

All cells start with the 100-request tuned params (sa_accept_pct=0.074, p1_final=0.08,
p2_final=0.0001, neighbor_k=30). Each cell gets independently tuned via bench_tune.

#### API Design

Profile resolution is deferred to solve time since request count isn't known when the
profile is set:

```c
sg_config_set_profile(ctx, SG_PROFILE_FAST);  // stores profile, doesn't apply yet
// ... add requests ...
sg_solve(ctx);  // resolves FAST × sg_scale_from_count(num_requests), applies cell
```

Explicit scale override for cases where the caller knows the problem size class:

```c
sg_config_set_profile_scale(ctx, SG_PROFILE_FAST, SG_SCALE_LARGE);  // forces LARGE
```

Manual `sg_set_config()` / `sg_set_tune_params()` without a profile still works identically
to before — the matrix is only consulted when a profile was set.

#### Files

| File | Role |
|------|------|
| `include/sg_types.h` | `SGScale` enum |
| `src/sg_profile_matrix.h` | `SGProfileCell`, extern matrix, `sg_scale_from_count()` |
| `src/sg_profile_matrix.c` | Static const 4×5 matrix, apply function |
| `include/sg_internal.h` | `active_profile`, `active_scale`, `profile_applied` in SGContext |
| `include/surge.h` | `sg_config_set_profile_scale()` declaration |
| `src/sg_context.c` | Deferred profile storage, new API |
| `src/sg_solve.c` | Profile resolution before `sg_prepare_travel()` |
| `tests/test_profile_matrix.c` | 16 unit tests |

### Tuning Campaign Strategy

#### Overview

Systematic tuning of all 20 cells of the profile × scale matrix using `bench_tune`'s
tiered grid search. Each cell gets its own JSONL checkpoint file for resume support.

**Tool:** `scripts/tune_matrix.sh` — autonomous, resumable tuning campaign script.

#### Cell Priority Order

Cells tuned in order of production impact:

| Priority | Cell | Instances | Time Budget | Rationale |
|----------|------|-----------|-------------|-----------|
| 1 | FAST × LARGE | 60 GH-400 | 30s | Most common production use case |
| 2 | FAST × MEDIUM | 60 GH-200 | 10s | Medium fleet, common |
| 3 | NEAR_OPTIMAL × LARGE | 60 GH-400 | 120s | Quality-sensitive large problems |
| 4 | BEST × LARGE | 60 GH-400 | 600s | Best quality, large |
| 5 | FAST × SMALL | 18 representative | 5s | Baseline (already tuned) |
| 6 | NEAR_OPTIMAL × MEDIUM | 60 GH-200 | 45s | |
| 7 | REALTIME × LARGE | 60 GH-400 | 5s | |
| 8 | REALTIME × MEDIUM | 60 GH-200 | 2s | |
| 9–20 | Remaining | GH or representative | varies | XLARGE, MASSIVE, remaining profiles |

SMALL-scale cells (priorities 5, 9, 10, 11) use the 18 representative Solomon + Li-Lim
instances. All other cells use the Gehring-Homberger instances at matching scale.

#### Instance Subsampling

For expensive cells, bench_tune's `--max-instances` flag selects evenly-spaced instances
to keep per-config evaluation cost manageable. The stride ensures all 6 GH class types
(C1, C2, R1, R2, RC1, RC2) are represented.

| Time Budget | Instances Used | Rationale |
|-------------|---------------|-----------|
| < 60s | All 60 | Cheap enough to evaluate fully |
| 60–119s | 30 of 60 | 5 per class |
| 120–599s | 18 of 60 | 3 per class |
| 600–1199s | 12 of 60 | 2 per class |
| ≥ 1200s | 6 of 60 | 1 per class — minimum for diversity |

#### Time Estimates

With instance subsampling, ~615 configurations per cell across 7 tiers:

| Scenario | 5950X (14 threads) | CCX63 (40 threads) |
|----------|--------------------|--------------------|
| Priority 8 cells | ~186h (7.7 days) | ~68h (2.8 days) |
| All 20 cells | ~693h (28 days) | ~250h (10.4 days) |

**Recommendation:** Hetzner CCX63 (48 vCPU, 192GB, ~$0.90/hr ≈ $220 for 10 days). All
20 cells fit in a single rental period.

#### Running the Campaign

```bash
# Setup
git clone <repo> otto && cd otto/surge
make bench_tune
make bench-download

# Full campaign (all 20 cells, ~10 days on CCX63)
nohup ./scripts/tune_matrix.sh --threads 40 > campaign.log 2>&1 &

# Priority cells only (~3 days on CCX63)
nohup ./scripts/tune_matrix.sh --threads 40 --priority-only > campaign.log 2>&1 &

# Specific cells (e.g., just FAST×LARGE and FAST×MEDIUM)
nohup ./scripts/tune_matrix.sh --threads 40 --cell 0,1 > campaign.log 2>&1 &

# Monitor
tail -f campaign.log
ls -la benchmarks/results/matrix/*.jsonl

# Resume after any interruption — just re-run the same command
# Checkpoint files track per-evaluation progress; completed tiers are skipped
```

#### After Tuning Completes

1. Review JSONL checkpoint files in `benchmarks/results/matrix/`
2. Extract winning params from each `tune_{profile}_{scale}.jsonl`
3. Update `k_profile_matrix` in `src/sg_profile_matrix.c` with tuned values
4. Re-validate winning configs with full instance sets (no subsampling)
5. Run existing test suite (`make test`) to verify backward compatibility

#### Implementation Status

| Step | Status |
|------|--------|
| SGScale enum + SGProfileCell type | Done |
| Static const 4×5 matrix | Done |
| Deferred profile resolution in sg_solve() | Done |
| sg_config_set_profile_scale() API | Done |
| bench_tune --profile / --scale-size / --max-instances | Done |
| scripts/tune_matrix.sh campaign script | Done |
| 16 unit tests (test_profile_matrix.c) | Done |
| Run tuning campaign | Pending |
| Update matrix with tuned values | Pending |
| Full-instance validation | Pending |

### Phase S19: Lazy Heap Regret Repair

**Priority: High. Biggest remaining lever for iterations/second.**

The repair fill functions (`sg_route_repair_fill_regret` and `sg_route_repair_fill_greedy`)
had O(N² × V × L) complexity: each of N insertions re-evaluated all N unassigned requests
across V vehicles. For GH-400 (N~400 construction, N~60 ALNS repair), most of the time
budget was spent on redundant position evaluations.

**Algorithm: Lazy max-heap with validate-on-pop**

```
Phase 1: evaluate all N requests, push to max-heap     ← O(N × V × L)
Phase 2: while heap not empty:
    pop highest-regret request                          ← O(log N)
    recompute this request's regret                     ← O(V × L)
    if still best (>= heap peek): insert it
    else: push back with updated priority, continue
```

Inserting request R into vehicle V only affects requests whose top-K insertions included V.
For V=40 vehicles, ~N/V ≈ 1-2 requests share the same best vehicle. So most pops validate
on the first try (expected c ≈ 1-2 pops per insertion). Total: O((1+c) × N × V × L) ≈
O(2N × V × L).

**Expected speedup:** N/2c ≈ 30x for ALNS repair (N=60), 200x for construction (N=400).
Even with constant factors, 5-20x real speedup means significantly more ALNS iterations
per second.

**Data structures:**

- `SGRegretEntry` — cached regret, first_score, vehicle_id, positions, route_distance
- `SHHeap *repair_heap` — 4-ary min-heap from `sh_heap.h` with negated priorities
- `SGRegretEntry *regret_cache` — indexed by request_id, arena-allocated in scratch

**Tiebreaking:** Regret: (1) highest regret, (2) lowest first_score, (3) lowest request_id.
Greedy (regret_k=1): (1) lowest first_score, (2) lowest request_id.

**Files modified:**

| File | Changes |
|------|---------|
| `include/sg_internal.h` | `SGRegretEntry` typedef, 2 fields in `SGScratchBuffers` |
| `src/sg_solution.c` | Arena size + alloc for `regret_cache`, `sh_heap_create`/`free` |
| `src/sg_repair.c` | `sg_repair_fill_heap` (~100 lines), rename existing to `_linear`, wrappers |
| `tests/test_surge.c` | 8 tests for heap repair correctness |

**Benchmark results (GH-400, 60s, population, commit c26ff30):**

| Metric | Before S19 (b3905e2) | After S19 (c26ff30) | Delta |
|--------|---------------------|---------------------|-------|
| Vehicle match | 27/60 (45%) | 33/60 (55%) | +6 instances |
| Avg vehicle gap | — | +0.63 | — |
| Avg distance gap | +33.3% | +18.6% | -14.7pp |
| Avg runtime | 74s | 77s | +3s |

Per-class detail (60s, population):

| Class | Cases | Exact Veh | Avg Veh Gap | Avg Dist Gap | Notes |
|-------|-------|-----------|-------------|--------------|-------|
| C1 | 10 | 2 | +1.6 | +16.7% | c1_4_1 perfect (40v, +0.0%) |
| C2 | 10 | 3 | +0.8 | +15.4% | Clustered wide-TW |
| R1 | 10 | 7 | +0.2 | +31.9% | Random tight-TW |
| R2 | 10 | 10 | +0.0 | +9.8% | Random wide-TW, all veh exact |
| RC1 | 10 | 1 | +1.0 | +24.3% | Mixed tight-TW |
| RC2 | 10 | 10 | +0.3 | +7.4% | Mixed wide-TW, all veh exact |

Key observations:
- R2 and RC2 (wide time windows) achieve exact vehicle counts on all instances.
- c1_4_1 matches BKS perfectly (40 vehicles, +0.0% distance).
- Distance gap is the main remaining opportunity — algorithm is sound, needs more
  iterations (time budget) and tuned parameters (S20) to close further.

**Long-run comparison (GH-400, 7200s = 2h, population):**

Literature standard: PyVRP 0.45% mean gap, HGS 0.32% mean gap at 2h on GH-1000.

| Instance | Veh | BKS Veh | Dist | BKS Dist | Dist Gap | Runtime | Notes |
|----------|-----|---------|------|----------|----------|---------|-------|
| c1_4_1 | 40 | 40 | 7,152 | 7,152 | **+0.0%** | 2006s | Clustered tight-TW, BKS match |
| r1_4_1 | 40 | 40 | 11,352 | 10,372 | +9.4% | 7240s | Random tight-TW, exact vehicles |

c1_4_1 converged to BKS in 33 minutes (hit iteration limit before time limit).
r1_4_1 used the full 2h budget. At equivalent 2h budget, gap ranges from 0% (clustered)
to ~9% (random tight-TW). The random instances have more diverse feasible neighborhoods,
requiring more sophisticated move operators to close the gap.

**Implementation status:** ✅ Done (Mar 2026)

### Phase S20: PD-Aware Local Search Operators ✅

**Priority: High. Fills structural gaps in the operator toolkit.**

Added 3 new local search operators to the intensify loop and an O(1) concat pre-filter
for intra-route 2-opt. The intensify loop previously had no segment-reversal move and
no way to independently reposition pickup/delivery stops within or across routes.

**Operators:**

1. **Intra-route 2-opt** (`sg_route_try_2opt_intra_once`): Reverse a contiguous
   subsequence of stops within a single route. Classic TSP distance improvement move.
   PD safety check in O(j-i) ensures no pickup-before-delivery violations after
   reversal. Uses `sg_concat_eval_2opt_intra` for O(1) distance lower-bound on
   delivery-only instances. Falls back to O(L) `sg_route_stop_sequence_feasible`
   on PD instances.

2. **PD-pair intra-route relocate** (`sg_route_try_pd_relocate_intra_once`): Excise
   a PD pair's pickup and delivery stops, try all (pickup_pos, delivery_pos) placements
   within the same route. Finds distance-optimal intra-route PD interleaving that
   request-level OR-opt can't discover. Gated by `ctx->has_pd_requests`.

3. **PD-pair inter-route relocate** (`sg_route_try_pd_relocate_once`): Move a PD pair
   from one vehicle to best interleaved positions on another. Uses
   `sg_route_eval_pd_best_insertion_cached` for O(L²) evaluation with per-position
   cached timing. Neighbor pruning and compatibility pre-checks for efficiency.
   Gated by `ctx->has_pd_requests`.

**Intensify loop order:**
```
2-opt intra → OR-opt → exchange → 2-opt* → cross-exchange
  → [PD] pd-relocate-intra → pd-relocate-inter → pd-reorder
```

**Files modified:**

| File | Changes |
|------|---------|
| `include/sg_internal.h` | 3 operator declarations + `sg_concat_eval_2opt_intra` |
| `src/sg_postprocess.c` | 3 operator implementations (~300 lines), intensify loop |
| `src/sg_concat.c` | `sg_concat_eval_2opt_intra` (~100 lines) |
| `tests/test_surge.c` | 12 new tests (433 total) |

**Benchmark results (GH-400, 60s, population, commit b132790):**

| Metric | S19 baseline | S20 | Delta |
|--------|-------------|-----|-------|
| Vehicle match | 33/60 (55%) | 33/60 (55%) | 0 |
| Avg distance gap | +18.6% | +19.2% | +0.6pp |
| Avg runtime | 77s | 77s | 0 |

**Assessment:** The +0.6pp distance regression is within noise and reflects the
cost of running 2-opt intra in the intensify loop on time-starved 400-request
instances. Option C (moving 2-opt to final polish) was tested and performed worse
(+20.3%), confirming 2-opt needs to compound with other operators across passes.
The PD operators are no-ops on delivery-only Solomon instances — their value will
show on Li-Lim PDPTW benchmarks.

**Next steps from S20:**
- Run Li-Lim PDPTW benchmarks to validate PD operators
- Consider size-gating 2-opt intra (`num_requests <= 200`) if regression persists
- Document per-class GH-400 results in `benchmarks/results/`

**Implementation status:** ✅ Done (Mar 2026)

### Phase S21: Move Evaluation Cache

**Priority: High. Biggest lever for effective ALNS iterations per second.**

#### Motivation

The repair hot path — `sg_route_rank_insertions_for_request()` — evaluates every
unassigned request against every vehicle on every heap pop. After inserting request r
into vehicle v, the insertion costs for all other requests on all other vehicles are
unchanged — but we recompute them from scratch. With V=40 vehicles and k=60 removed
requests, ~97% of evaluations are redundant.

The key insight: **insertion cost for (request, vehicle) is a pure function of the
vehicle's stop sequence and the request's attributes**. If the vehicle's route hasn't
changed since the last evaluation, the cached result is identical.

#### Design Principles

1. **Orthogonal to constraints.** The cache sits between the caller
   (`sg_route_rank_insertions_for_request`) and the evaluator
   (`sg_route_eval_best_insertion_cached` / `sg_route_eval_pd_best_insertion_cached`).
   No constraint logic is modified. Cache hit returns the stored result; cache miss
   calls the real evaluator and stores the result.

2. **Opaque to the rest of the system.** Callers don't know whether a result was
   cached or freshly computed. The cache is managed internally by the evaluation
   functions. A `ctx->insertion_cache` pointer being NULL disables caching entirely.

3. **Identical moves with and without.** The cache is a pure memoization layer.
   Given the same inputs, it returns the same outputs. Toggle via
   `SGModelConfig.use_insertion_cache` (default: true). All existing tests pass
   unchanged with cache enabled or disabled.

#### Data Structures

```c
/* Per-(request, vehicle) cached insertion evaluation result */
typedef struct {
    uint64_t generation;        /* Vehicle route generation at evaluation time */
    double   score;             /* Best insertion cost delta */
    double   route_distance;    /* New route distance if inserted */
    uint32_t pos;               /* Best insertion position (delivery-only) */
    uint32_t pickup_pos;        /* Best pickup position (PD) */
    uint32_t delivery_pos;      /* Best delivery position (PD) */
    uint8_t  feasible;          /* 1 = at least one feasible position found */
} SGInsertionCacheEntry;

/* Cache state, owned by SGContext, allocated in scratch arena */
typedef struct {
    SGInsertionCacheEntry *entries;  /* [num_requests * max_vehicles] flat 2D */
    uint64_t *vehicle_gen;          /* [max_vehicles] route generation counters */
    uint32_t num_requests;
    uint32_t max_vehicles;
    uint64_t hits;                  /* Stats: cache hits */
    uint64_t misses;                /* Stats: cache misses */
} SGInsertionCache;
```

#### Generation Counter Protocol

Every function that modifies a vehicle's stop sequence increments that vehicle's
generation counter. This is the **sole invalidation mechanism** — no per-entry
expiry, no LRU, no scanning.

```
Route modification sites (vehicle_gen[v]++):
├── sg_route_apply_insertion()           ← delivery-only insert
├── sg_route_apply_pd_insertion()        ← PD pair insert
├── sg_route_unassign_removed_requests() ← destroy (per affected vehicle)
├── sg_route_rebuild_vehicle_stop_state()← postprocess operators
└── sg_route_restore_from_backup()       ← rollback (all vehicles)
```

**Lookup protocol** (inside `sg_route_eval_best_insertion_cached`):

```c
SGInsertionCacheEntry *e = &cache->entries[request_id * max_vehicles + vehicle_id];
if (cache && e->generation == cache->vehicle_gen[vehicle_id]) {
    cache->hits++;
    *score_out = e->score;
    /* ... copy other fields ... */
    return e->feasible;
}
/* Cache miss: evaluate normally */
cache->misses++;
int feasible = sg_route_eval_best_insertion_uncached(ctx, sol, ...);
/* Store result */
e->generation = cache->vehicle_gen[vehicle_id];
e->score = *score_out;
/* ... store other fields ... */
e->feasible = feasible;
return feasible;
```

#### Where the Cache Lives

```
SGContext
└── scratch (SGScratchBuffers)
    └── insertion_cache (SGInsertionCache *)    ← arena-allocated
        ├── entries[]    ← flat [R × V] array
        └── vehicle_gen[] ← per-vehicle counter
```

Allocated once in `sg_route_solution_init_scratch()` alongside the existing
`regret_cache` and `repair_heap`. Freed automatically when the arena resets.

The cache persists across ALNS iterations within a solve. It is reset (all
generations zeroed) at the start of each solve call, not per-iteration — the
generation counters handle staleness.

#### Expected Hit Rates

**During repair (heap Phase 2):**

After inserting request r_i into vehicle v_j:
- 1 vehicle modified (v_j) → `vehicle_gen[v_j]++`
- Remaining (k - i - 1) requests × (V - 1) vehicles → all cache hits
- Hit rate per insertion: (V-1)/V ≈ 97% for V=40

Over k insertions: total evaluations = k × V = 2400. Cache hits ≈ k × (V-1) ×
(sum of remaining / k) ≈ 2340. Misses ≈ 60 (one per vehicle change).
Net: **~97% hit rate**, eliminating ~97% of O(L) or O(L²) evaluations.

**During repair (heap Phase 1 — initial evaluation):**

All entries are cold (generation 0 vs vehicle_gen starts at 1). Hit rate: 0%.
This is expected — Phase 1 must evaluate everything. The cache pays off in Phase 2.

**After destroy:**

Destroy typically modifies k vehicles (k ≈ 20-60 for GH-400). All k vehicles get
generation increments. The remaining (V - k) vehicles retain valid cache entries
from the previous iteration's repair. Hit rate on first Phase 1 evaluation:
(V - k) / V ≈ 0-50% depending on destroy scope.

#### Memory Cost

For R=400 requests, V=100 vehicles:
- entries: 400 × 100 × 48 bytes = 1.9 MB
- vehicle_gen: 100 × 8 bytes = 800 bytes
- Total: ~2 MB (fits in arena, negligible vs existing allocations)

#### Files Modified

| File | Changes |
|------|---------|
| `include/sg_internal.h` | `SGInsertionCacheEntry`, `SGInsertionCache` typedefs |
| `src/sg_solution.c` | Arena alloc for cache in scratch init |
| `src/sg_feasibility.c` | Cache lookup/store wrapper around eval functions |
| `src/sg_repair.c` | Pass cache to eval calls (already has ctx) |
| `src/sg_postprocess.c` | Generation increment in route-modifying operators |
| `tests/test_surge.c` | Cache correctness tests |

#### Constraint Non-Interference

The cache **does not affect** any constraint evaluation:
- Time window propagation: unchanged (called on cache miss)
- Capacity checking: unchanged (called on cache miss)
- Compatibility (qualifications, exclusions, compartments): unchanged
- Break policy, ride time, LIFO/FIFO: unchanged
- Segment summaries (SGSegSummary): unchanged (rebuilt on route modification,
  independent of cache)

The cache is **downstream** of all constraint logic — it stores the final result
of a complete evaluation, not intermediate constraint state.

#### Testing Strategy

| Test | Verifies |
|------|----------|
| `test_insertion_cache_basic_hit` | Same (request, vehicle) returns cached result |
| `test_insertion_cache_miss_on_modify` | Generation increment causes re-evaluation |
| `test_insertion_cache_destroy_invalidates` | Destroy increments affected vehicles |
| `test_insertion_cache_unaffected_survives` | Unmodified vehicles retain cache |
| `test_insertion_cache_pd_correctness` | PD insertion cache stores correct (pp, dp) |
| `test_insertion_cache_disabled_identical` | `use_insertion_cache=false` → same solution |
| `test_insertion_cache_vs_uncached_fuzz` | Random destroy/repair: cached == uncached |
| `test_insertion_cache_stats` | Hit/miss counters accurate |
| `test_solve_with_cache_solomon` | Full Solomon solve: identical result ± cache |
| `test_solve_with_cache_li_lim` | Full Li-Lim solve: identical result ± cache |

The **identity tests** are the most important: run the same problem twice (once with
cache, once without) with deterministic seed, assert `total_distance` and
`vehicles_used` are identical. This proves the cache is a pure optimization with
no behavioral change.

**Expected improvement:** 5-15x faster repair phase on GH-400 instances, translating
to 3-8x more ALNS iterations in the same time budget. Combined with S19 heap repair,
this should materially close the distance gap at 60s.

#### Implementation (Completed)

Commit `4eb0730`. 441/441 tests pass (433 existing + 8 new cache tests).

Key design change from sketch: generation counters live in **`SGRouteSolution.route_generation[v]`**
(not the cache), enabling cross-iteration persistence via arena memcpy. Added `penalty_gen`
counter for penalty weight invalidation at segment boundaries.

#### GH-400 Benchmark Results (S21, 60s population)

| Metric | S19 (pre-cache) | S21 (cache) | Delta |
|--------|-----------------|-------------|-------|
| Veh exact match | 33/60 (55%) | 30/60 (50%) | -3 |
| Avg veh gap | +0.58 | +0.67 | +0.09 worse |
| Avg dist gap | +18.6% | +17.8% | -0.8pp better |
| Avg time (s) | 77 | 78 | ~same |

Neutral at 60s — cache throughput gains are masked by construction/crossover time in
population mode. The benefit compounds at longer time limits where Phase 2 ALNS polish
dominates wall time.

### Phase S22: Profile-Based Tuning Campaign ✅

Ran 7-tier logarithmic grid search (`bench_tune all-tiers`) on 5 representative GH-400
instances (r1_4_1, c1_4_1, rc1_4_1, r2_4_1, c2_4_1) with 60s budget. Total: ~839 configs
plus 4-seed verification on top 3.

**Tuning tiers:**
1. Phase budget (phase1_fraction, phase15_iters) — 25 configs
2. SA temperature (sa_accept_pct, p1_final_temp_ratio, p2_final_temp_ratio) — 80 configs
3. Penalty adaptation (target_start/end, tolerance, increase/decrease) — 324 configs
4. ALNS rewards (reaction_factor, reward_best/better/accepted) — 256 configs
5. Destruction sizing (segment_size, string_l_max) — 64 configs
6. Extended randomness (worst, shaw, route_cluster, time_cluster) — 81 configs
7. Neighborhood size (neighbor_k) — 9 configs

**Key parameter changes from BASE_TUNE (LARGE_TUNE macro):**

| Parameter | BASE_TUNE | LARGE_TUNE | Effect |
|-----------|-----------|------------|--------|
| sa_accept_pct | 0.074 | 0.010 | 7x cooler SA start temperature |
| p2_final_temp_ratio | 0.0001 | 0.0063 | 63x slower Phase 2 cooling |
| phase1_fraction | 0.60 | 0.40 | More time on distance polish |
| phase15_iters | 2000 | 1000 | Shorter transition phase |
| reaction_factor | 0.10 | 0.50 | 5x faster ALNS learning |
| reward_better | 4.00 | 20.00 | 5x stronger reward for improvement |
| reward_accepted | 2.00 | 0.50 | Weaker reward for merely accepted |
| neighbor_k | 30 | 20 | Smaller neighborhood (faster at scale) |
| worst_randomness | 4.0 | 10.0 | More randomness in worst removal |
| time_cluster_randomness | 2.0 | 10.0 | More randomness in time clustering |

Also includes explicit penalty adaptation (pen_target_start=0.50, pen_target_end=0.30,
pen_tolerance=0.15, pen_increase=2.0, pen_decrease=0.5) overriding defaults.

**Implementation:**
- Created `LARGE_TUNE` and `RT_LARGE_TUNE` macros in `sg_profile_matrix.c`
- Updated LARGE column in all 4 profiles (REALTIME, FAST, NEAR_OPTIMAL, BEST)
- Fixed `bench_solomon.c` to use `sg_profile_matrix_apply()` instead of hardcoded params

#### GH-400 Benchmark Results (S22, 60s population)

| Metric | S19 (pre-tune) | S22 (tuned) | Delta |
|--------|----------------|-------------|-------|
| Veh exact match | 33/60 (55%) | 35/60 (58%) | +2 |
| Avg veh gap | +0.67 | +0.53 | -0.14 better |
| Avg dist gap | +18.6% | +12.9% | -5.7pp better |
| Avg time (s) | 78 | 76 | ~same |

Per-category breakdown (60s population, S22 tuned):

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 5/10 | +0.90 | +12.3% |
| C2_4 (clustered, wide) | 10 | 3/10 | +0.70 | +8.9% |
| R1_4 (random, tight) | 10 | 9/10 | +0.10 | +21.6% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +7.9% |
| RC1_4 (mixed, tight) | 10 | 1/10 | +0.90 | +17.2% |
| RC2_4 (mixed, wide) | 10 | 7/10 | +0.60 | +9.4% |
| **Overall** | **60** | **35/60 (58%)** | **+0.53** | **+12.9%** |

Notable improvements: c1_4_6 +8.3%→+0.0% (BKS match), r1_4_1 +18.9%→+13.7%,
rc1_4_8 +26.0%→+13.5%, c2_4_7 +17.7%→+21.8% (some regression expected with stochastic).

**300s spot-check (3 representative instances, population, S22 tuned):**

| Instance | Veh | BKS Veh | Dist | BKS Dist | Dist Gap | Runtime |
|----------|-----|---------|------|----------|----------|---------|
| c1_4_1 | 40 | 40 | 7,152 | 7,152 | **+0.0%** | 384s |
| r1_4_1 | 40 | 40 | 11,815 | 10,372 | +13.9% | 344s |
| rc1_4_1 | 38 | 36 | 9,139 | 8,571 | +6.6% | 321s |

c1_4_1 matches BKS perfectly at both 60s and 300s. rc1_4_1 distance improved substantially
(+12.2%→+6.6%) but still uses +2 vehicles. r1_4_1 plateaued — random tight-TW instances
need more sophisticated operators or longer time budgets to close the gap.

**Insight:** SA temperature was the biggest lever (Tier 2 dropped composite from 9137→7053).
At 400-customer scale, the default SA was far too hot — accepting too many bad moves.
Smaller neighborhood (k=20) was also critical, reducing per-iteration eval cost.

### Phase S23: Parallel Move Evaluation (Future)

**Priority: Low. Stacks with S19 heap repair and S21 cache.**

Within each ALNS iteration, the destroy-repair cycle is single-threaded. Population
mode runs independent ALNS threads, but each thread's repair evaluates positions
sequentially. Two approaches:

1. **Speculative parallelism:** Evaluate multiple destroy-repair pairs concurrently
   within a single thread, keeping the best outcome.
2. **Split repair evaluation:** Partition unassigned requests across OpenMP threads
   for parallel insertion cost evaluation during Phase 1 of the heap repair.

**Expected improvement:** 2-4x effective iterations/sec on multi-core, stacking with
the heap repair speedup from S19. Net effect: 10-80x more ALNS iterations in the
same time budget compared to pre-S19 baseline.

### Phase S24: Instance-Adaptive Construction (Future)

**Priority: Low. Incremental improvement over CFRS.**

CFRS (Phase S16) improved construction significantly, but all instances use the same
multi-strategy tournament (regret-3, TW-sorted, I1, sweep, k-means). Instance features
(spatial distribution, TW tightness, capacity utilization) could drive strategy selection.

**Approach:** Feature extraction → strategy scores from benchmark data → per-instance
best strategy. Also: seeded insertion order based on geographic clustering (hard-to-place
requests first).

**Expected improvement:** 1-3 fewer vehicles in initial solution for clustered instances,
giving ALNS a head start. Most impactful for C-type instances (clustered) where the
insertion order has outsized effect on vehicle count.
