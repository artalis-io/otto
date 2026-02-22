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

### Depot Constraints

| Constraint | Description |
|------------|-------------|
| **Multiple depots** | Vehicles assigned to different depots |
| **Open routes** | End anywhere (not return to depot) |
| **Depot capacity** | Max vehicles dispatched per depot |
| **Depot time windows** | Loading dock availability |

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

### Current Status (as of 2026-02-22)

**Baseline**: U1-U8 + S1-S9 complete. All usability phases done. 109 tests passing, ASAN/UBSAN clean. Benchmarks unchanged from previous baseline (soft TWs not active in benchmark instances — zero impact on existing behavior).

Best measured quality (10000 iterations, deterministic seed 42):
- Solomon (VRPTW, 56 cases): `solved=56/56`, `avgVehGap=+0.38`, `avgDistGap=+0.2%`, `equalVehicles=35`, `lexiNonWorse=11`.
- Li & Lim (PDPTW, 57 cases): `solved=57/57`, `avgVehGap=+0.59`, `avgDistGap=+3.9%`, `equalVehicles=39`, `lexiNonWorse=21`.

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

Best measured quality (10000 iterations, deterministic seed 42):
- Solomon (VRPTW, 56 cases): `avgVehGap=+0.36`, `avgDistGap=-0.2%`, `equalVehicles=36`, `lexiNonWorse=11`.
- Li & Lim (PDPTW, 56 cases): `avgVehGap=+0.55`, `avgDistGap=+4.1%`, `equalVehicles=40`, `lexiNonWorse=19`.
- All 113 solutions verified feasible (post-solve validation gate in `sg_solve_route_model`).

Solver quality highlights:
- Wide-TW instances (C2, LC2, LR2, LRC2) essentially solved — nearly all match BKS on both vehicles and distance.
- Solomon C1xx: 8/9 match BKS distance exactly.
- Remaining gap: tight-TW random instances (R1, LR1, RC1, LRC1) consistently use +1 vehicle; LC101/LC102 use +4-5 vehicles (identical TW widths defeat sorting heuristics).

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
- [ ] Add per-operator telemetry reporting in benchmark output for focused tuning.

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

### Phase 5: Objective and Acceptance Modernization
- [ ] Move from scalar proxy toward explicit lexicographic compare (unassigned -> vehicles -> distance -> soft penalties).
- [ ] Expose acceptance policy in `SGConfig` (SA/RRT/Improving) and tune per problem class.
- [ ] Add adaptive destroy size policy based on request count and stagnation.

### Phase 6: Rich Constraint Completion
- [ ] Disjunct TW support.
- [x] Soft TW penalties and waiting-cost terms in objective.
- [x] Vehicle qualifications (U2).
- [ ] Commodity conflicts, exclusion groups.
- [x] Open routes (U4).
- [ ] Depot-level dispatch constraints.
- [x] Max route duration (U5).
- [ ] HoSE/break constraints (with Tempo/HoSE integration).

- [x] Add optional travel-time/distance matrix API and use it in construction + route feasibility (U1).
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
- Unified route state drives both delivery-only and PDPTW solves. The stop-based kernel tracks forward/backward time slack, load profiles, and ride-time constraints.
- Stop-level splice/excise operations preserve non-adjacent PD placement across ALNS destroy/repair cycles.

### Phase 8: Verification and Benchmark Expansion
- [ ] Keep Solomon VRPTW as regression benchmark (already wired).
- [ ] Add Li & Lim PDPTW harness and BKS comparator.
- [ ] Add Cordeau DARP harness (including ride-time constraints).
- [ ] Expand unit tests from smoke coverage to operator and feasibility regression suites.
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

With solver quality at a production-usable level (Solomon -0.2% avg distance gap, Li & Lim
+4.1%), the focus shifts to modeling real-world constraints. These phases are ordered by
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

With U1-U6 complete, the following gaps remain between Surge and a production-ready solver.
Grouped by business impact:

**Tier 1 — Blocking for production:**

| Gap | Impact | Effort |
|-----|--------|--------|
| **Soft time windows (U7)** ✅ | Per-task soft TW with linear penalty within hard bounds. No feasibility kernel changes needed — penalty layer only. 4 tests. | Medium |
| **Disjunct time windows** | Customer availability often has multiple windows (e.g., 8-12 and 14-18). Reworks forward/backward pass to evaluate union of intervals. | Large |
| **Driver breaks / HoS** | Legal requirement in EU/US trucking. Requires break insertion points in routes and HoSE state machine integration. | Large |

**Tier 2 — High business value:**

| Gap | Impact | Effort |
|-----|--------|--------|
| **Waiting cost** ✅ | Penalize early arrival. Per-vehicle `cost_per_waiting` coefficient, accumulated in forward pass, added to objective. 3 tests. | Small |
| **Overtime cost** ✅ | Penalize work beyond shift end. Per-vehicle `cost_per_overtime` coefficient with soft shift boundary. 3 tests. | Small |
| **Depot dispatch limits** | Max vehicles per depot. Global constraint — can't check locally per insertion. | Medium |
| **Multiple trips per vehicle** | Depot reload between trips. Fundamentally different route representation. | Large |

**Tier 3 — Niche / specialized:**

| Gap | Impact | Effort |
|-----|--------|--------|
| **Commodity conflicts** | Hazmat ∉ same vehicle as food. Bitmask tracking per stop. | Medium |
| **Exclusion groups** | Requests that cannot share a vehicle. Per-route tracking. | Medium |
| **Sequence-dependent setup** | Cleanup time between incompatible cargo types. | Medium |
| **Time-dependent travel** | Rush hour matrices. Multiple matrix sets indexed by departure time. | Large |

### Future (not planned yet)

These are real-world features that require larger architectural changes:

| Feature | Why deferred |
|---------|-------------|
| **Disjunct time windows** | Changes TW from a single interval to a union — significant feasibility kernel rework |
| **Driver breaks / HoSE** | Requires break insertion points in routes, variable-length stop sequences, HoSE state machine |
| **Multiple trips** | Requires multi-route-per-vehicle state, depot reload modeling, fundamentally different route representation |
| **Commodity conflicts** | Requires tracking commodity state along the route (bitmask per stop), conflict checking at insertion |
| **Request exclusion groups** | Requires per-route exclusion tracking, expensive to check incrementally |
| **Depot dispatch limits** | Requires global constraint across vehicles — can't check locally per insertion |

---

## Solver Profiles

Three built-in iteration profiles for different use cases. The API default is 1000 (batch).
Users can override via `SGConfig.max_iterations` or `--iterations` in benchmarks.

| Profile | Iterations | Runtime (100-req) | Solomon distGap | Li & Lim distGap | Use case |
|---------|-----------|-------------------|-----------------|------------------|----------|
| **Real-time** | 300 | ~0.2 s | +5.5% | +9.0% | API responses, live dispatch |
| **Batch** (default) | 1,000 | ~0.6 s | +3.0% | +5.0% | Daily planning, route optimization |
| **High quality** | 5,000 | ~2.0 s | +0.2% | +4.3% | Offline analysis |
| **Best quality** | 10,000 | ~4.5 s | -0.2% | +4.1% | Benchmarking, maximum quality |

### Iteration Scaling Data (100-customer instances, deterministic seed 42)

**Solomon (VRPTW, 56 cases)**:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 100 | 0.10 | +10.9% | +1.11 | 16 | 4 |
| 300 | 0.22 | +5.5% | +0.86 | 21 | 6 |
| 500 | 0.51 | +4.0% | +0.84 | 20 | 7 |
| 1,000 | 0.66 | +3.0% | +0.77 | 23 | 8 |
| 2,000 | 0.95 | +2.0% | +0.59 | 27 | 9 |
| 5,000 | 1.83 | +0.8% | +0.50 | 31 | 11 |
| 10,000 | 4.58 | -0.2% | +0.36 | 36 | 11 |

**Li & Lim (PDPTW, 56 cases)**:

| Iters | Sec/case | avgDistGap | avgVehGap | equalVeh | lexiNonWorse |
|------:|--------:|-----------:|----------:|---------:|-------------:|
| 100 | 0.09 | +12.8% | +1.32 | 16 | 5 |
| 300 | 0.13 | +9.0% | +1.02 | 27 | 10 |
| 500 | 0.35 | +6.5% | +0.93 | 29 | 12 |
| 1,000 | 0.60 | +5.0% | +0.77 | 33 | 14 |
| 2,000 | 0.99 | +5.0% | +0.73 | 33 | 17 |
| 5,000 | 2.13 | +4.2% | +0.71 | 34 | 18 |
| 10,000 | 3.25 | +4.1% | +0.55 | 40 | 19 |

**Observations**:
- The improvement knee is at ~1000 iterations for both benchmarks.
- Solomon continues to improve log-linearly through 5000; Li & Lim plateaus at ~1000, indicating structural gaps (operators, not search time) are the bottleneck for PDPTW.
- Runtime scales sub-linearly: 5000 iters costs ~18x of 100 iters (not 50x) due to fixed construction/postprocess costs.

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
