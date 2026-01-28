# Project-Level Feature TODOs

This document outlines planned features at the project level, including new components and infrastructure changes.

## Table of Contents

1. [Project Renaming](#1-project-renaming)
2. [HoSE - Hours of Service Engine](#2-hose---hours-of-service-engine)
3. [Tempo - Business Rules Engine](#3-tempo---business-rules-engine)
4. [Arbor - State-Space Search Engine](#4-arbor---state-space-search-engine)

---

## 1. Project Renaming

### Current State

The project is currently named "ralph" (after the LP solver component), but the scope has expanded significantly beyond refueling optimization:

| Component | Purpose |
|-----------|---------|
| `ralph/` | LP/MIP solver engine |
| `fuelwise/` | Refueling optimization domain logic |
| `velo/` | Routing engine (Dijkstra, A*, landmarks) |
| `carta/` | Map tile generation (MVT, PNG) |
| `shared/` | Common geo utilities |
| `hose/` | **Planned**: Hours of Service rule engine |
| `tempo/` | **Planned**: Business rules / time window engine |
| `arbor/` | **Planned**: State-space search framework |

### Problem

- "ralph" implies LP solver only
- "fuelwise" implies refueling only
- The project is evolving into a comprehensive trucking/logistics optimization platform

### Proposed Names

Consider renaming the project root to reflect its broader scope:

| Name | Meaning | Pros | Cons |
|------|---------|------|------|
| **artalis** | From "Artalis.io" (company name) | Brand consistency | Generic |
| **haul** | Trucking theme | Short, memorable | Common word |
| **convoy** | Fleet/logistics theme | Evocative | Existing company |
| **freightkit** | Freight + toolkit | Descriptive | Long |
| **logix** | Logistics + algorithms | Short, techy | Spelling confusion |
| **truckstack** | Trucking software stack | Very descriptive | Long, corporate |

### TODOs

- [ ] Decide on new project name
- [ ] Update all documentation references
- [ ] Update CLAUDE.md files across components
- [ ] Update Makefile targets
- [ ] Update Docker image names
- [ ] Update GitHub repository name (if applicable)
- [ ] Update import paths in code

---

## 2. HoSE - Hours of Service Engine

**H**ours **o**f **S**ervice **E**ngine

### Overview

A rule engine for computing driver Hours of Service (HoS) compliance under US FMCSA and EU EC/561 regulations. Given a driver's current state and a required driving time, HoSE computes:

1. **Transit Duration**: Total time including mandatory breaks/rest
2. **End Driver State**: Updated clocks and status after transit
3. **ETA**: Estimated time of arrival accounting for breaks
4. **Action Sequence**: List of drive/break/rest actions with timestamps

### Regulations Covered

#### US FMCSA (Federal Motor Carrier Safety Administration)

| Rule | Description |
|------|-------------|
| 11-Hour Driving | Max 11 hours driving after 10 consecutive hours off duty |
| 14-Hour Window | No driving after 14 hours on-duty following 10 hours off |
| 30-Minute Break | Required after 8 cumulative hours of driving |
| 60/70-Hour Limit | Max 60 hours in 7 days or 70 hours in 8 days |
| 34-Hour Restart | Resets weekly limit after 34+ consecutive hours off |
| Sleeper Berth | Split sleeper provisions (7/3 or 8/2 splits) |

#### EU EC/561 (European Regulations)

| Rule | Description |
|------|-------------|
| 4.5-Hour Driving | Max 4.5 hours before 45-minute break |
| 9-Hour Daily | Max 9 hours daily (extendable to 10 hours twice/week) |
| 56-Hour Weekly | Max 56 hours in single week |
| 90-Hour Fortnightly | Max 90 hours in any two consecutive weeks |
| 11-Hour Daily Rest | Min 11 hours rest in 24-hour period (reducible to 9 hours 3x/week) |
| 45-Hour Weekly Rest | Min 45 hours weekly rest (reducible to 24 hours every other week) |
| Ferry/Train Rule | Rest during ferry/train crossings |

### High-Level Architecture

```
hose/
├── include/
│   ├── hose.h              # Public API
│   ├── hs_types.h          # Data structures (driver state, actions)
│   ├── hs_fmcsa.h          # US FMCSA rule definitions
│   └── hs_ec561.h          # EU EC/561 rule definitions
├── src/
│   ├── hose.c              # Main API implementation
│   ├── hs_fmcsa.c          # FMCSA rule engine
│   ├── hs_ec561.c          # EC/561 rule engine
│   ├── hs_state.c          # Driver state management
│   └── hs_schedule.c       # Action sequence generation
├── tests/
│   └── test_hose.c         # Unit tests
└── CLAUDE.md
```

### Core Data Structures (Preliminary)

```c
// hs_types.h

typedef enum {
    HS_RULESET_FMCSA,       // US Federal
    HS_RULESET_EC561,       // EU
    HS_RULESET_CANADA,      // Future: Canadian HoS
} HSRuleset;

typedef enum {
    HS_STATUS_OFF_DUTY,
    HS_STATUS_SLEEPER,
    HS_STATUS_DRIVING,
    HS_STATUS_ON_DUTY_NOT_DRIVING,
} HSStatus;

typedef enum {
    HS_ACTION_DRIVE,
    HS_ACTION_BREAK,
    HS_ACTION_REST,
    HS_ACTION_ON_DUTY,
} HSActionType;

/* Driver's current HoS state */
typedef struct {
    HSRuleset ruleset;
    HSStatus current_status;

    /* Clocks (in seconds) */
    double driving_today;           /* Driving time in current shift */
    double on_duty_today;           /* On-duty time in current shift */
    double driving_since_break;     /* Driving since last 30-min break (FMCSA) */
    double time_in_current_status;  /* Time in current status */

    /* Weekly tracking */
    double driving_this_week;       /* Total driving this week */
    double on_duty_this_week;       /* Total on-duty this week */
    int days_since_restart;         /* Days since 34-hour restart (FMCSA) */

    /* EU-specific */
    double driving_this_fortnight;  /* For 90-hour rule */
    int extended_days_this_week;    /* 10-hour days used (max 2) */
    int reduced_rests_this_week;    /* 9-hour rests used (max 3) */

    /* Timestamps */
    time_t shift_start;             /* When current shift started */
    time_t last_break_end;          /* When last qualifying break ended */

} HSDriverState;

/* A single action in the schedule */
typedef struct {
    HSActionType type;
    double duration;                /* Duration in seconds */
    time_t start_time;              /* Absolute start time */
    time_t end_time;                /* Absolute end time */
    char description[64];           /* Human-readable description */
} HSAction;

/* Result of transit computation */
typedef struct {
    double net_driving_time;        /* Requested driving time */
    double total_transit_time;      /* Total time including breaks */
    time_t eta;                     /* Estimated time of arrival */

    HSDriverState end_state;        /* Driver state after transit */

    HSAction *actions;              /* Sequence of actions */
    int num_actions;

    int feasible;                   /* 1 if transit is feasible, 0 if impossible */
    char infeasibility_reason[128]; /* Why infeasible (if applicable) */
} HSTransitResult;
```

### Core API (Preliminary)

```c
// hose.h

/**
 * Initialize a driver state with default values
 */
void hs_init_state(HSDriverState *state, HSRuleset ruleset);

/**
 * Compute transit result for given driving time
 *
 * @param state         Current driver state
 * @param net_driving   Required driving time in seconds
 * @param start_time    When transit begins
 * @param result        Output: computed transit result
 * @return              0 on success, -1 on error
 */
int hs_compute_transit(const HSDriverState *state, double net_driving,
                       time_t start_time, HSTransitResult *result);

/**
 * Apply an action to driver state (update clocks)
 */
void hs_apply_action(HSDriverState *state, const HSAction *action);

/**
 * Check if driver can legally drive for given duration
 */
int hs_can_drive(const HSDriverState *state, double duration);

/**
 * Get required break/rest before driving can continue
 */
HSAction hs_get_required_break(const HSDriverState *state);

/**
 * Free transit result resources
 */
void hs_free_result(HSTransitResult *result);
```

### Algorithm Sketch

```
compute_transit(state, net_driving, start_time):
    actions = []
    current_state = copy(state)
    current_time = start_time
    remaining_driving = net_driving

    while remaining_driving > 0:
        # How long can we drive before hitting a limit?
        max_drive = min(
            remaining_driving,
            driving_limit - current_state.driving_today,
            window_limit - current_state.on_duty_today,
            break_threshold - current_state.driving_since_break
        )

        if max_drive <= 0:
            # Must take break or rest
            required_break = get_required_break(current_state)
            actions.append(required_break)
            apply_action(current_state, required_break)
            current_time += required_break.duration
        else:
            # Drive
            drive_action = Action(DRIVE, max_drive, current_time)
            actions.append(drive_action)
            apply_action(current_state, drive_action)
            current_time += max_drive
            remaining_driving -= max_drive

    return TransitResult(
        total_time = current_time - start_time,
        eta = current_time,
        end_state = current_state,
        actions = actions
    )
```

### Integration Points

| Component | Integration |
|-----------|-------------|
| **FuelWise** | Include HoS breaks in refueling route planning |
| **Velo** | Add HoS-aware travel time estimation |
| **API** | Expose HoS computation as REST endpoint |
| **UI** | Display break schedule and ETA with HoS |

### Future Extensions

- **ELD Integration**: Parse Electronic Logging Device data to initialize state
- **Multi-Driver**: Team driving rules
- **Exceptions**: Short-haul, adverse conditions, agricultural exemptions
- **Optimization**: Find optimal break placement to minimize total transit time
- **Canadian HoS**: Add Canadian federal and provincial rules

### TODOs

- [ ] Finalize data structures for driver state
- [ ] Implement FMCSA rule engine
- [ ] Implement EC/561 rule engine
- [ ] Create comprehensive test suite with edge cases
- [ ] Document rule interpretations and assumptions
- [ ] Integrate with FuelWise for break-aware routing
- [ ] Add API endpoint for HoS computation
- [ ] Consider WASM build for browser-side computation

### References

- [FMCSA Hours of Service Regulations](https://www.fmcsa.dot.gov/regulations/hours-service/summary-hours-service-regulations)
- [Regulation (EC) No 561/2006](https://eur-lex.europa.eu/legal-content/EN/ALL/?uri=CELEX%3A32006R0561)
- [FMCSA ELD Mandate](https://www.fmcsa.dot.gov/hours-service/elds/electronic-logging-devices)

---

## 3. Tempo - Business Rules Engine

**T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator

### Overview

A constraint evaluation engine for business rules that go beyond HoS regulations. Tempo handles operational constraints like time windows, appointment scheduling, facility hours, and custom business policies.

### Constraint Types

| Category | Examples |
|----------|----------|
| **Time Windows** | Delivery windows (e.g., 9am-5pm), pickup appointments |
| **Facility Hours** | Warehouse open hours, gate restrictions, weekend closures |
| **Service Times** | Loading/unloading durations, dwell time requirements |
| **Appointment Slots** | Fixed appointment times, slot-based scheduling |
| **Blackout Periods** | Holidays, restricted hours (e.g., no deliveries 2am-6am) |
| **Lead Times** | Minimum advance notice for appointments |
| **Capacity Limits** | Max trucks per hour at facility, dock door limits |

### High-Level Architecture

```
tempo/
├── include/
│   ├── tempo.h             # Public API
│   ├── tp_types.h          # Data structures
│   ├── tp_window.h         # Time window operations
│   ├── tp_calendar.h       # Business calendar/hours
│   └── tp_constraint.h     # Constraint definitions
├── src/
│   ├── tempo.c             # Main API implementation
│   ├── tp_window.c         # Time window logic
│   ├── tp_calendar.c       # Calendar computations
│   ├── tp_constraint.c     # Constraint evaluation
│   └── tp_feasibility.c    # Feasibility checking
├── tests/
│   └── test_tempo.c        # Unit tests
└── CLAUDE.md
```

### Core Concepts (Preliminary)

```c
// tp_types.h

/* Time window (half-open interval [start, end)) */
typedef struct {
    time_t start;
    time_t end;
} TPTimeWindow;

/* Recurring schedule (e.g., Mon-Fri 9am-5pm) */
typedef struct {
    uint8_t days_of_week;       /* Bitmask: bit 0 = Sunday */
    int start_hour, start_min;  /* Daily start time */
    int end_hour, end_min;      /* Daily end time */
    time_t effective_from;      /* Schedule valid from */
    time_t effective_until;     /* Schedule valid until */
} TPRecurringSchedule;

/* Facility with operating hours */
typedef struct {
    int id;
    char name[64];
    TPRecurringSchedule *schedules;
    int num_schedules;
    TPTimeWindow *blackouts;    /* Holiday closures, etc. */
    int num_blackouts;
} TPFacility;

/* Constraint on an activity */
typedef struct {
    enum {
        TP_CONSTRAINT_TIME_WINDOW,      /* Must occur within window */
        TP_CONSTRAINT_APPOINTMENT,      /* Must start at exact time */
        TP_CONSTRAINT_FACILITY_HOURS,   /* Facility must be open */
        TP_CONSTRAINT_MIN_DWELL,        /* Minimum time at location */
        TP_CONSTRAINT_MAX_DWELL,        /* Maximum time at location */
        TP_CONSTRAINT_LEAD_TIME,        /* Minimum advance notice */
    } type;

    union {
        TPTimeWindow window;
        time_t appointment_time;
        TPFacility *facility;
        double dwell_seconds;
        double lead_time_seconds;
    } data;

    int is_hard;                /* Hard constraint vs soft (preference) */
    double penalty;             /* Penalty for soft constraint violation */
} TPConstraint;

/* Result of constraint evaluation */
typedef struct {
    int feasible;
    int num_violations;
    TPConstraint **violated;    /* Which constraints violated */
    double total_penalty;       /* Sum of soft constraint penalties */
    time_t earliest_feasible;   /* Earliest time activity can start */
    time_t latest_feasible;     /* Latest time activity can start */
} TPEvalResult;
```

### Core API (Preliminary)

```c
// tempo.h

/**
 * Check if a time falls within facility operating hours
 */
int tp_is_facility_open(const TPFacility *facility, time_t when);

/**
 * Find next time facility opens after given time
 */
time_t tp_next_opening(const TPFacility *facility, time_t after);

/**
 * Evaluate constraints for an activity at given time
 */
TPEvalResult tp_evaluate(const TPConstraint *constraints, int num_constraints,
                         time_t activity_start, double activity_duration);

/**
 * Find feasible window for activity given constraints
 */
int tp_find_feasible_window(const TPConstraint *constraints, int num_constraints,
                            time_t earliest, time_t latest,
                            double activity_duration,
                            TPTimeWindow *result);

/**
 * Intersect multiple time windows
 */
int tp_intersect_windows(const TPTimeWindow *windows, int num_windows,
                         TPTimeWindow *result);
```

### Integration Points

| Component | Integration |
|-----------|-------------|
| **FuelWise** | Fuel stop must be during station hours |
| **HoSE** | Break locations must be accessible |
| **Velo** | Route planning with time-dependent constraints |
| **API** | Constraint definition and evaluation endpoints |

### TODOs

- [ ] Define core data structures for time windows and constraints
- [ ] Implement time window intersection/union operations
- [ ] Implement recurring schedule evaluation
- [ ] Implement facility hours checking
- [ ] Implement constraint evaluation engine
- [ ] Add soft constraint penalty calculation
- [ ] Integrate with HoSE for combined feasibility
- [ ] Add API endpoints for constraint management
- [ ] Create test suite with realistic scenarios

---

## 4. Arbor - State-Space Search Engine

**A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime

### Overview

A generic state-space search framework for solving complex combinatorial problems through systematic exploration. Arbor provides the infrastructure for branching, pruning, bounding, and state management that can be specialized for different problem domains.

### Use Cases

| Problem | State | Branching | Pruning |
|---------|-------|-----------|---------|
| Vehicle Routing | Partial route + unvisited stops | Add next stop | Bound vs best known |
| Scheduling | Partial schedule + unassigned tasks | Assign task to slot | Constraint violation |
| Bin Packing | Partial packing + remaining items | Place item in bin | Capacity exceeded |
| Trip Planning | Current location + remaining legs | Choose next leg | HoS/time window violation |

### High-Level Architecture

```
arbor/
├── include/
│   ├── arbor.h             # Public API
│   ├── ar_types.h          # Data structures
│   ├── ar_state.h          # State management interface
│   ├── ar_branch.h         # Branching strategies
│   ├── ar_bound.h          # Bounding functions
│   └── ar_search.h         # Search algorithms
├── src/
│   ├── arbor.c             # Main API implementation
│   ├── ar_search_dfs.c     # Depth-first search
│   ├── ar_search_bfs.c     # Breadth-first search
│   ├── ar_search_best.c    # Best-first search
│   ├── ar_search_beam.c    # Beam search
│   ├── ar_search_bnb.c     # Branch and bound
│   ├── ar_pool.c           # State pool management
│   └── ar_stats.c          # Search statistics
├── tests/
│   └── test_arbor.c        # Unit tests
└── CLAUDE.md
```

### Core Concepts (Preliminary)

```c
// ar_types.h

/* Opaque state handle - actual state defined by problem domain */
typedef struct ARState ARState;

/* Branch: a choice point in the search tree */
typedef struct {
    int branch_id;
    void *branch_data;          /* Problem-specific branching data */
    double priority;            /* For best-first ordering */
    char description[64];       /* Human-readable description */
} ARBranch;

/* Bound result */
typedef enum {
    AR_BOUND_FEASIBLE,          /* State may lead to feasible solution */
    AR_BOUND_PRUNED,            /* State cannot improve on best known */
    AR_BOUND_INFEASIBLE,        /* State violates hard constraints */
} ARBoundResult;

/* Search result */
typedef enum {
    AR_RESULT_OPTIMAL,          /* Proven optimal found */
    AR_RESULT_FEASIBLE,         /* Feasible solution found (not proven optimal) */
    AR_RESULT_INFEASIBLE,       /* No feasible solution exists */
    AR_RESULT_LIMIT,            /* Hit time/node/memory limit */
} ARSearchResult;

/* Search statistics */
typedef struct {
    uint64_t nodes_explored;
    uint64_t nodes_pruned;
    uint64_t nodes_infeasible;
    uint64_t solutions_found;
    double best_objective;
    double best_bound;
    double gap;
    double elapsed_seconds;
} ARStats;

/* Search parameters */
typedef struct {
    int max_nodes;
    double time_limit;
    double gap_tolerance;
    int solution_limit;
    enum {
        AR_STRATEGY_DFS,        /* Depth-first (memory efficient) */
        AR_STRATEGY_BFS,        /* Breadth-first (level by level) */
        AR_STRATEGY_BEST_FIRST, /* Priority queue by bound */
        AR_STRATEGY_BEAM,       /* Limited width BFS */
        AR_STRATEGY_DIVING,     /* DFS with periodic restarts */
    } strategy;
    int beam_width;             /* For beam search */
    int verbose;
} ARParams;

/* Problem-specific callbacks */
typedef struct {
    /* Create initial state */
    ARState* (*init)(void *problem_data);

    /* Free state */
    void (*free_state)(ARState *state);

    /* Clone state */
    ARState* (*clone)(const ARState *state);

    /* Check if state is complete solution */
    int (*is_complete)(const ARState *state);

    /* Get objective value (lower is better for minimization) */
    double (*objective)(const ARState *state);

    /* Compute lower bound on best achievable from this state */
    double (*lower_bound)(const ARState *state);

    /* Generate branches (children) from state */
    int (*branch)(const ARState *state, ARBranch **branches, int *num_branches);

    /* Apply branch to state (returns new state) */
    ARState* (*apply_branch)(const ARState *state, const ARBranch *branch);

    /* Check feasibility (can prune early) */
    ARBoundResult (*check_feasibility)(const ARState *state);

    /* Free branch data */
    void (*free_branch)(ARBranch *branch);

    /* Optional: custom pruning beyond bound comparison */
    int (*should_prune)(const ARState *state, double best_known);

    /* Optional: dominance check (state1 dominates state2?) */
    int (*dominates)(const ARState *state1, const ARState *state2);

} ARCallbacks;
```

### Core API (Preliminary)

```c
// arbor.h

/* Search context */
typedef struct ARContext ARContext;

/**
 * Create search context
 */
ARContext* ar_create(const ARCallbacks *callbacks, void *problem_data);

/**
 * Free search context
 */
void ar_free(ARContext *ctx);

/**
 * Run search
 */
ARSearchResult ar_search(ARContext *ctx, const ARParams *params);

/**
 * Get best solution found
 */
ARState* ar_get_best_solution(const ARContext *ctx);

/**
 * Get search statistics
 */
ARStats ar_get_stats(const ARContext *ctx);

/**
 * Set incumbent (warm start with known solution)
 */
void ar_set_incumbent(ARContext *ctx, ARState *solution, double objective);

/**
 * Add callback for solution found events
 */
void ar_on_solution(ARContext *ctx, void (*callback)(ARState *solution, void *user_data),
                    void *user_data);
```

### Search Strategies

**Depth-First Search (DFS)**
```
- Memory efficient: O(depth) states in memory
- Finds solutions quickly but may not be optimal
- Good for finding any feasible solution
```

**Best-First Search**
```
- Explores most promising states first (by lower bound)
- Optimal for branch-and-bound
- Higher memory usage: O(nodes) states in memory
```

**Beam Search**
```
- Limited-width BFS: keep only top-k states per level
- Good balance of quality and memory
- Not guaranteed optimal
```

**Diving with Restarts**
```
- DFS to find solutions quickly
- Periodically restart from best unexplored state
- Hybrid of exploration and exploitation
```

### Integration Points

| Component | Use Case |
|-----------|----------|
| **FuelWise** | Route optimization with refueling decisions |
| **HoSE** | Finding feasible break schedules |
| **Tempo** | Scheduling with time window constraints |
| **Ralph** | Complement LP/MIP for combinatorial subproblems |

### Example: Trip Planning Search

```c
/* State: current location, time, HoS state, remaining stops */
typedef struct {
    int current_stop;
    time_t current_time;
    HSDriverState driver_state;
    int *remaining_stops;
    int num_remaining;
    double total_cost;
} TripState;

/* Branch: choose next stop */
ARBranch* trip_branch(const ARState *state, int *num) {
    TripState *ts = (TripState*)state;
    *num = ts->num_remaining;
    ARBranch *branches = malloc(*num * sizeof(ARBranch));
    for (int i = 0; i < *num; i++) {
        branches[i].branch_id = ts->remaining_stops[i];
        branches[i].priority = /* distance or time to stop */;
    }
    return branches;
}

/* Lower bound: MST on remaining stops */
double trip_lower_bound(const ARState *state) {
    TripState *ts = (TripState*)state;
    return ts->total_cost + mst_cost(ts->remaining_stops, ts->num_remaining);
}
```

### TODOs

- [ ] Define state and callback interface
- [ ] Implement state pool with efficient allocation
- [ ] Implement DFS search
- [ ] Implement best-first search with priority queue
- [ ] Implement beam search
- [ ] Add node limit, time limit, gap tolerance stopping
- [ ] Add search statistics collection
- [ ] Implement warm start with incumbent
- [ ] Add solution callback mechanism
- [ ] Implement dominance-based pruning (optional)
- [ ] Create example applications (TSP, scheduling)
- [ ] Integrate with other components for combined optimization

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
| `api/` | Active | REST API server |
| `ui/` | Active | React frontend |

## Implementation Priority

1. **Project Renaming** - Low priority (cosmetic, do when convenient)
2. **HoSE Core** - High priority (essential for realistic trucking optimization)
3. **Tempo Core** - High priority (time windows needed for realistic planning)
4. **Arbor Core** - Medium priority (enables advanced optimization)
5. **Component Integration** - High priority (HoSE + Tempo + FuelWise)
