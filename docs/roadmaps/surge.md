# Surge - PDPTW Solver Architecture

**S**cheduler for **U**rban **R**outing and **G**eneral **E**xpress

ALNS-based solver for Pickup and Delivery Problems with Time Windows, built on Arbor.

## Problem Definition

### PDPTW Components

```
Requests:  { (pickup_i, delivery_i, load_i, tw_pickup_i, tw_delivery_i) }
Vehicles:  { (capacity_k, depot_k, shift_start_k, shift_end_k) }
Objective: Minimize total cost (distance + time + vehicles used)

Constraints:
- Precedence: pickup_i before delivery_i on same route
- Pairing: pickup_i and delivery_i on same vehicle
- Capacity: sum(loads on vehicle) ≤ capacity at all times
- Time windows: arrival ∈ [early, late] for each stop
- Shift: route starts/ends within vehicle shift window
```

### Complexity

| Instance Size | Requests | Approach |
|--------------|----------|----------|
| Small | < 50 | Exact (Ralph MIP) feasible |
| Medium | 50-500 | ALNS required |
| Large | 500-5000 | ALNS + parallelization |
| Very Large | 5000+ | Decomposition + ALNS |

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Surge API                              │
│  sg_create() │ sg_solve() │ sg_add_request() │ sg_get_solution() │
├──────────────────────────────────────────────────────────────────┤
│                        ALNS Controller                            │
│  Operator selection (roulette) │ Acceptance │ Termination        │
├────────────────────┬─────────────────────────────────────────────┤
│   Destroy Operators │              Repair Operators               │
│  ┌────────────────┐ │ ┌────────────────┐ ┌─────────────────────┐ │
│  │ Random Remove  │ │ │ Greedy Insert  │ │ Regret-k Insert     │ │
│  │ Worst Remove   │ │ │ Best Insert    │ │ Sequential Insert   │ │
│  │ Related Remove │ │ │ Cheapest Insert│ │ Parallel Insert     │ │
│  │ Shaw Remove    │ │ └────────────────┘ └─────────────────────┘ │
│  │ Cluster Remove │ │                                            │
│  │ Route Remove   │ │                                            │
│  └────────────────┘ │                                            │
├────────────────────┴─────────────────────────────────────────────┤
│                      Solution Representation                      │
│  Routes[] → Stops[] → { request_id, type, arrival, departure }   │
├──────────────────────────────────────────────────────────────────┤
│                         Evaluation Layer                          │
│  Feasibility check │ Cost delta │ TW propagation │ Capacity check │
├──────────────────────────────────────────────────────────────────┤
│                     Infrastructure (Existing)                     │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────────────────────┐│
│  │  Arbor  │ │  Velo   │ │  Ralph  │ │         Shared           ││
│  │ Search  │ │ Routing │ │  MIP    │ │ Geo, Heap, HashMap, Arena││
│  └─────────┘ └─────────┘ └─────────┘ └──────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

---

## Data Structures

### Core Types

```c
/* Request: pickup-delivery pair with time windows */
typedef struct {
    uint32_t id;

    /* Locations (indices into location array) */
    uint32_t pickup_loc;
    uint32_t delivery_loc;

    /* Load (positive = pickup adds, delivery removes) */
    int32_t load;

    /* Time windows [early, late] in seconds from midnight */
    int32_t pickup_early, pickup_late;
    int32_t delivery_early, delivery_late;

    /* Service times at each location */
    int32_t pickup_service;
    int32_t delivery_service;

    /* Priority (for regret calculation) */
    int32_t priority;
} SGRequest;

/* Vehicle with capacity and shift constraints */
typedef struct {
    uint32_t id;
    uint32_t depot_loc;
    int32_t capacity;
    int32_t shift_start, shift_end;
    double cost_per_km;
    double cost_per_hour;
    double fixed_cost;
} SGVehicle;

/* Location with coordinates */
typedef struct {
    uint32_t id;
    double lat, lon;
    const char *name;
} SGLocation;

/* Stop in a route */
typedef struct {
    uint32_t request_id;
    uint8_t type;            /* SG_STOP_PICKUP, SG_STOP_DELIVERY, SG_STOP_DEPOT */
    int32_t arrival;         /* Computed arrival time */
    int32_t departure;       /* arrival + service + wait */
    int32_t load_after;      /* Cumulative load after this stop */
} SGStop;

/* Route: sequence of stops for one vehicle */
typedef struct {
    uint32_t vehicle_id;
    uint32_t num_stops;
    uint32_t capacity;       /* Allocated capacity */
    SGStop *stops;           /* Array of stops (including depot start/end) */
    double distance;         /* Total route distance */
    double duration;         /* Total route duration */
    double cost;             /* Total route cost */
} SGRoute;

/* Solution: set of routes */
typedef struct {
    uint32_t num_routes;
    uint32_t num_unassigned;
    SGRoute *routes;
    uint32_t *unassigned;    /* Request IDs not yet assigned */
    double total_cost;
    double total_distance;
    int64_t iteration;
} SGSolution;
```

### Problem Context

```c
typedef struct {
    /* Problem data */
    uint32_t num_requests;
    uint32_t num_vehicles;
    uint32_t num_locations;
    SGRequest *requests;
    SGVehicle *vehicles;
    SGLocation *locations;

    /* Distance/time matrix (precomputed or on-demand via Velo) */
    double *dist_matrix;     /* num_locations × num_locations */
    double *time_matrix;     /* num_locations × num_locations */
    bool matrix_precomputed;
    VLGraph *road_graph;     /* For on-demand routing */

    /* ALNS state */
    SGSolution current;
    SGSolution best;
    SGALNSState alns;

    /* Configuration */
    SGConfig config;

    /* Memory arena for allocations */
    Arena arena;
} SGContext;
```

---

## ALNS Algorithm

### Main Loop

```c
SGStatus sg_solve_alns(SGContext *ctx) {
    /* Initialize with greedy construction */
    sg_construct_initial(ctx);
    sg_copy_solution(&ctx->best, &ctx->current);

    /* ALNS parameters */
    double temperature = ctx->config.initial_temp;
    const double cooling_rate = ctx->config.cooling_rate;
    const int max_iter = ctx->config.max_iterations;
    const int segment_size = ctx->config.segment_size;

    for (int iter = 0; iter < max_iter; iter++) {
        /* Select operators via roulette wheel */
        int destroy_op = sg_select_operator(ctx->alns.destroy_weights, NUM_DESTROY_OPS);
        int repair_op = sg_select_operator(ctx->alns.repair_weights, NUM_REPAIR_OPS);

        /* Copy current solution to working copy */
        SGSolution candidate;
        sg_copy_solution(&candidate, &ctx->current);

        /* Destroy: remove q requests */
        int q = sg_compute_removal_count(ctx, iter);
        uint32_t removed[q];
        ctx->alns.destroy_ops[destroy_op](ctx, &candidate, q, removed);

        /* Repair: reinsert removed requests */
        ctx->alns.repair_ops[repair_op](ctx, &candidate, q, removed);

        /* Evaluate and accept/reject */
        double delta = candidate.total_cost - ctx->current.total_cost;

        if (sg_accept(delta, temperature, ctx)) {
            sg_copy_solution(&ctx->current, &candidate);

            if (ctx->current.total_cost < ctx->best.total_cost) {
                sg_copy_solution(&ctx->best, &ctx->current);
                sg_update_operator_weight(ctx, destroy_op, repair_op, SG_REWARD_BEST);
            } else {
                sg_update_operator_weight(ctx, destroy_op, repair_op, SG_REWARD_BETTER);
            }
        } else {
            sg_update_operator_weight(ctx, destroy_op, repair_op, SG_REWARD_REJECTED);
        }

        /* Update temperature */
        temperature *= cooling_rate;

        /* Normalize weights every segment */
        if (iter % segment_size == 0) {
            sg_normalize_weights(ctx);
        }

        /* Early termination check */
        if (sg_should_terminate(ctx, iter)) break;
    }

    return SG_STATUS_OK;
}
```

### Acceptance Criteria

```c
/* Simulated Annealing acceptance */
bool sg_accept_sa(double delta, double temperature, SGContext *ctx) {
    if (delta < 0) return true;  /* Always accept improvements */

    double prob = exp(-delta / temperature);
    return sg_random_double(ctx) < prob;
}

/* Record-to-Record Travel (deterministic) */
bool sg_accept_rrt(double delta, double threshold, SGContext *ctx) {
    return (ctx->current.total_cost + delta) < (ctx->best.total_cost + threshold);
}

/* Great Deluge */
bool sg_accept_gd(double delta, double water_level, SGContext *ctx) {
    return (ctx->current.total_cost + delta) < water_level;
}
```

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

/* Problem building */
uint32_t sg_add_location(SGContext *ctx, double lat, double lon, const char *name);
uint32_t sg_add_vehicle(SGContext *ctx, uint32_t depot_loc, int capacity,
                         int shift_start, int shift_end);
uint32_t sg_add_request(SGContext *ctx,
                         uint32_t pickup_loc, uint32_t delivery_loc,
                         int load,
                         int pickup_early, int pickup_late,
                         int delivery_early, int delivery_late,
                         int pickup_service, int delivery_service);

/* Optional: set road graph for Velo routing */
void sg_set_road_graph(SGContext *ctx, VLGraph *graph);

/* Solving */
SGStatus sg_solve(SGContext *ctx);

/* Solution access */
int sg_get_num_routes(SGContext *ctx);
int sg_get_route_stops(SGContext *ctx, int route_idx,
                        uint32_t *request_ids, uint8_t *types, int max_stops);
double sg_get_total_cost(SGContext *ctx);
double sg_get_total_distance(SGContext *ctx);
int sg_get_num_unassigned(SGContext *ctx);
const uint32_t *sg_get_unassigned(SGContext *ctx);

/* Export */
char *sg_solution_to_json(SGContext *ctx);

#endif /* SURGE_H */
```

---

## Implementation Plan

### Phase 1: Core (1 week)
- [ ] Data structures (SGRequest, SGVehicle, SGRoute, SGSolution)
- [ ] Problem builder API
- [ ] Greedy construction heuristic
- [ ] Basic feasibility checking
- [ ] Distance matrix (Euclidean initially)

### Phase 2: ALNS Framework (1 week)
- [ ] ALNS main loop
- [ ] Operator selection (roulette wheel)
- [ ] Simulated annealing acceptance
- [ ] Basic destroy: random, worst
- [ ] Basic repair: greedy, regret-2

### Phase 3: Advanced Operators (1 week)
- [ ] Shaw removal (related)
- [ ] Route removal
- [ ] Regret-k insertion
- [ ] Time window propagation optimization

### Phase 4: Integration (1 week)
- [ ] Velo integration (distance/time matrices)
- [ ] Arbor integration (parallel evaluation)
- [ ] API server endpoints
- [ ] JSON input/output
- [ ] WASM compilation

### Phase 5: Testing & Optimization (1 week)
- [ ] Unit tests (feasibility, operators)
- [ ] Benchmark on standard instances (Solomon, Li & Lim)
- [ ] Parameter tuning
- [ ] Performance optimization

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

1. Ropke & Pisinger (2006) - "An Adaptive Large Neighborhood Search Heuristic for the Pickup and Delivery Problem with Time Windows"
2. Shaw (1997) - "A New Local Search Algorithm Providing High Quality Solutions to Vehicle Routing Problems"
3. Li & Lim (2001) - PDPTW benchmark instances
4. Solomon (1987) - VRPTW benchmark instances

---

## Appendix: Standard Benchmark Instances

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

Use these for validation against published best-known solutions.
