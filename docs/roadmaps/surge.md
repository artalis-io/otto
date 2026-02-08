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

### Phase 1: Core Foundation (1-2 weeks)
- [ ] Core data structures (SGRequest, SGVehicle, SGRoute, SGSolution)
- [ ] Request types: PDPTW, VRPTW delivery, pickup, service
- [ ] Single-dimension capacity (weight only initially)
- [ ] Single time window per stop
- [ ] Problem builder API
- [ ] Greedy construction heuristic
- [ ] Basic feasibility checking (TW, capacity, precedence)
- [ ] Euclidean distance matrix

### Phase 2: Basic ALNS (1 week)
- [ ] Basic destroy: random, worst
- [ ] Basic repair: greedy, regret-2
- [ ] Arbor solution ops callbacks (copy, cost, free)
- [ ] Register operators with Arbor ALNS
- [ ] Unit tests for operators

### Phase 3: Rich Capacity Constraints (1 week)
- [ ] Multi-dimensional capacity (weight, volume, pallets, etc.)
- [ ] SGLoadVector operations (add, subtract, compare)
- [ ] Incremental capacity tracking on route
- [ ] Update feasibility checker for multi-dim

### Phase 4: Rich Time Constraints (1 week)
- [ ] Disjunct time windows (multiple windows per stop)
- [ ] Soft time windows with tardiness penalties
- [ ] Max ride time (DARP support)
- [ ] Waiting cost in objective
- [ ] Efficient TW propagation for disjunct windows

### Phase 5: Compatibility Constraints (1 week)
- [ ] Vehicle qualifications (refrigerated, ADR, tail-lift, etc.)
- [ ] Commodity types and conflicts
- [ ] Request exclusion groups
- [ ] Incremental commodity tracking on route
- [ ] Compatibility-aware insertion

### Phase 6: Multi-Depot & Route Types (1 week)
- [ ] Multiple depots with constraints
- [ ] Per-depot vehicle limits
- [ ] Open routes (no return to depot)
- [ ] Different start/end depots
- [ ] Depot time windows

### Phase 7: Advanced Operators (1 week)
- [ ] Shaw removal (distance + time + load + commodity similarity)
- [ ] Route removal
- [ ] Zone-based removal (geographic clusters)
- [ ] Regret-k insertion (k=2,3,4)
- [ ] Parallel insertion evaluation

### Phase 8: Integration (1 week)
- [ ] Velo integration (road distance/time matrices)
- [ ] Ralph integration (exact MIP for small instances)
- [ ] HoSE integration (driver breaks)
- [ ] API server endpoints
- [ ] JSON input/output
- [ ] WASM compilation

### Phase 9: Testing & Benchmarking (1-2 weeks)
- [ ] Unit tests for all constraint types
- [ ] Benchmark on Solomon VRPTW instances
- [ ] Benchmark on Li & Lim PDPTW instances
- [ ] Benchmark on Cordeau DARP instances
- [ ] Rich VRP instances (custom)
- [ ] Parameter tuning
- [ ] Performance profiling and optimization

**Note:** The ALNS framework (main loop, operator selection, acceptance criteria,
adaptive weights) is provided by Arbor. Surge implements VRP/PDPTW-specific operators
and constraint checking, integrating via Arbor's callback interface.

---

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
