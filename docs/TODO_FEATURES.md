# Project-Level Feature TODOs

This document outlines planned features at the project level, including new components and infrastructure changes.

## Table of Contents

1. [Project Renaming](#1-project-renaming)
2. [HoSE - Hours of Service Engine](#2-hose---hours-of-service-engine)
3. [Tempo - Business Rules Engine](#3-tempo---business-rules-engine)
4. [Arbor - State-Space Search Engine](#4-arbor---state-space-search-engine)
5. [Sigma - Fleet Plan Selection Engine](#5-sigma---fleet-plan-selection-engine)
6. [Pulse - Execution Tracker and PTA Engine](#6-pulse---execution-tracker-and-pta-engine)
7. [Distance and Duration Estimation](#7-distance-and-duration-estimation-cross-cutting)
8. [Cost and Profit Calculations](#8-cost-and-profit-calculations-cross-cutting)
9. [FuelWise Integration](#9-fuelwise-integration-refueling-in-search)

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
| `sigma/` | **Planned**: Fleet-wide plan selection (set covering MIP) |
| `pulse/` | **Planned**: Execution tracking and PTA computation |

### Problem

- "ralph" implies LP solver only
- "fuelwise" implies refueling only
- The project is evolving into a comprehensive trucking/logistics optimization platform

### Proposed Names

The name should sound like an animal or human name - catchy and memorable.

**Recommended (Human/Animal-like):**

| Name | Backronym | Why It Works |
|------|-----------|--------------|
| **OTTO** | **O**ptimization for **T**rucking and **T**ransport **O**perations | German name, evokes efficiency/engineering |
| **RUFUS** | **R**outing **U**tility for **F**leet **U**nified **S**cheduling | Friendly, memorable |
| **MARCO** | **M**apping **A**nd **R**oute **C**oordination **O**ptimizer | Like Marco Polo - exploration/routing |
| **FELIX** | **F**leet and **L**ogistics **I**ntelligence e**X**ecutive | Latin for "lucky/successful" |
| **ATLAS** | (no backronym needed) | Titan who carries the world - strong imagery |
| **ORCA** | **O**ptimization for **R**outing, **C**ompliance, and **A**llocation | Powerful, intelligent animal |
| **HAWK** | **H**aulage **A**nd **W**orkflow **K**ernel | Sharp, efficient predator |

**Other options:**

| Name | Meaning | Pros | Cons |
|------|---------|------|------|
| **artalis** | From "Artalis.io" (company name) | Brand consistency | Generic |
| **haul** | Trucking theme | Short, memorable | Common word |
| **convoy** | Fleet/logistics theme | Evocative | Existing company |

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
| Ferry/Train Rule | Rest during ferry/train crossings with caps |

**EU Clock Model Implications:**

Unlike FMCSA's 4-clock model, EU requires multiple overlapping clocks:
- Daily driving clock (4.5h before break, 9/10h total)
- Daily duty clock
- Weekly driving clock (56h)
- Bi-weekly driving clock (90h across 2 weeks)
- Daily rest counter
- Weekly rest counter

**EU-Specific State Tracking:**

```c
typedef struct {
    /* Driving clocks */
    double driving_since_break;     /* Reset by 45-min break */
    double driving_today;           /* 9h or 10h limit */
    double driving_this_week;       /* 56h limit */
    double driving_last_week;       /* For bi-weekly calc */

    /* Rest usage counters */
    int extended_driving_days;      /* 10h days used this week (max 2) */
    int reduced_daily_rests;        /* 9h rests used this week (max 3) */
    int reduced_weekly_rest_pending;/* Compensation required? */

    /* Split rest tracking */
    int split_rest_first_part_taken;/* For 3h+9h split */
    double split_rest_first_duration;

    /* Weekly rest */
    time_t last_weekly_rest_end;
    int was_reduced_weekly;         /* Was last weekly rest reduced? */
} HSEC561State;
```

**Implementation Note**: Treat EU HoS as a **separate mode** rather than trying to force-fit into FMCSA clocks. The shared interface should remain `hs_transit_and_loading()`-like, but the underlying rule engine must be EU-specific.

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

### FMCSA Four-Clock Model

The FMCSA HoS rules are best modeled as **four independent clocks**, each with specific reset conditions:

| Clock | Limit | Resets When | Purpose |
|-------|-------|-------------|---------|
| 8-hour break clock | 8 hours | 30+ minute off-duty break taken | Tracks driving time since last qualifying break |
| 11-hour driving clock | 11 hours | 10+ hour off-duty period | Daily driving limit within shift |
| 14-hour shift clock | 14 hours | 10+ hour off-duty period | On-duty window from shift start |
| 70-hour cycle clock | 70 hours | 34+ hour off-duty restart | 8-day rolling on-duty limit |

**Key insight**: Clocks advance independently. The 14-hour window runs on wall-clock time regardless of whether driver is working. The 70-hour clock uses per-day on-duty history for recaps.

### Pre/Post Trip Inspections

FMCSA requires inspection time that consumes on-duty (non-driving) time:
- **Pre-trip inspection**: 30 minutes before driving can begin
- **Post-trip inspection**: 15 minutes after arriving (if inspection is enabled)

These must be factored into transit duration calculations:
```c
typedef struct {
    int pre_trip_completed;     /* Has pre-trip been done this shift? */
    int post_trip_required;     /* Is post-trip inspection pending? */
    double pre_trip_duration;   /* Default: 1800 seconds (30 min) */
    double post_trip_duration;  /* Default: 900 seconds (15 min) */
} HSInspectionState;
```

### Scheduling Strategy

The core scheduling philosophy: **"Drive when you can, rest when you must."**

This means:
1. Advance time as driving whenever HoS permits
2. Insert breaks/rest only when a clock would be violated
3. Merge slack (appointment wait time) into off-duty when beneficial

### Transit + Loading Algorithm

The essential algorithm simulates a transit segment with optional loading:

```
transit_and_loading(state, segment, ops):
    # Phase 1: Simulate transit (pure driving + mandatory breaks)
    transit_result = simulate_transit(state, segment.drive_duration)

    # Phase 2: Compute slack to appointment window
    arrival_time = state.current_time + transit_result.duration
    if segment.has_appointment:
        slack = segment.appointment_start - arrival_time
    else:
        slack = 0

    # Phase 3: Merge slack into off-duty if beneficial
    if ops.MERGE_SLACK and slack > 0:
        # Convert slack (waiting) into off-duty time
        # This can help reset clocks or satisfy break requirements
        off_duty_time += min(slack, needed_for_reset)

    # Phase 4: Determine minimum off-duty before loading
    min_off_duty = 0
    if needs_10h_reset(state) and ops.EXTEND_OFF_DUTY_TO_10H:
        min_off_duty = max(min_off_duty, 10 * 3600)
    if needs_34h_restart(state) and ops.EXTEND_OFF_DUTY_TO_34H:
        min_off_duty = max(min_off_duty, 34 * 3600)
    if state.post_trip_required:
        min_off_duty = max(min_off_duty, state.post_trip_duration)

    # Phase 5: Perform waiting (off-duty) and loading (on-duty non-driving)
    apply_off_duty(state, off_duty_time)
    apply_loading(state, segment.loading_duration)

    # Phase 6: Return total duration and validity
    return TransitResult(
        total_duration = transit_result.duration + off_duty_time + segment.loading_duration,
        is_valid = check_appointment_window(arrival_time, segment),
        end_state = state
    )
```

### Transit Duration Approximation (for Search)

For use during tree search, a **lower-bound cache** provides fast transit duration estimates:
- Precomputes minimal transit durations for a **fresh driver** at fixed periods
- Ignores detailed appointment rules and loading
- Intended for **pruning**, not final scoring

```c
typedef struct {
    double *duration_by_distance;   /* duration[d] = time for d miles with fresh driver */
    int num_entries;
    double distance_step;           /* Granularity in miles */
} HSTransitCache;

/* Lower-bound: assumes fresh driver, no loading, no appointments */
double hs_transit_lower_bound(const HSTransitCache *cache, double distance);
```

### Pattern-Based Acceleration

For long-haul trips (multiple days), pattern detection accelerates HoS simulation:
- Detect repeating patterns: 11h drive → 10h rest → 11h drive → ...
- Skip simulating individual segments within pattern
- Jump directly to pattern exit point

This is critical for performance on multi-day routes.

### Scheduling Options (Flags)

```c
typedef enum {
    HS_OPS_EXTEND_OFF_DUTY_TO_10H = 1 << 0,  /* Allow extending to 10h reset */
    HS_OPS_EXTEND_OFF_DUTY_TO_34H = 1 << 1,  /* Allow extending to 34h restart */
    HS_OPS_MERGE_OFF_DUTIES       = 1 << 2,  /* Merge consecutive off-duty periods */
    HS_OPS_MERGE_SLACK            = 1 << 3,  /* Convert wait slack to off-duty */
    HS_OPS_LIMIT_REMAINING_ON_DUTY= 1 << 4,  /* Cap remaining on-duty after merge */
} HSTransitOps;
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

**Phase 1: Core FMCSA Implementation**
- [ ] Implement 4-clock driver state model
- [ ] Implement clock advancement rules
- [ ] Implement clock reset detection (10h, 34h)
- [ ] Implement 30-minute break rule
- [ ] Implement pre/post trip inspection tracking
- [ ] Create test suite for individual clock rules

**Phase 2: Transit + Loading Algorithm**
- [ ] Implement basic transit simulation
- [ ] Implement slack computation for appointments
- [ ] Implement slack-to-off-duty merging
- [ ] Implement scheduling operation flags (HS_OPS_*)
- [ ] Create test suite for transit scenarios

**Phase 3: Performance Optimization**
- [ ] Implement lower-bound transit cache
- [ ] Implement pattern-based acceleration for long-haul
- [ ] Benchmark and optimize hot paths

**Phase 4: EU EC/561 Implementation**
- [ ] Design EU clock model (separate from FMCSA)
- [ ] Implement daily/weekly driving limits
- [ ] Implement break rules (45-min after 4.5h)
- [ ] Implement rest rules (daily/weekly, reduced)
- [ ] Implement split rest tracking
- [ ] Create EU test suite

**Phase 5: Integration**
- [ ] Integrate with FuelWise for break-aware routing
- [ ] Integrate with Velo for travel time estimation
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
| **Exclusion Windows** | Times when pickup/delivery is forbidden |
| **Max Transit** | Maximum time/distance from one point to next |

### Appointment Window Types

Three common appointment patterns need support:

| Type | Description | Example |
|------|-------------|---------|
| **Continuous** | Single open-close window | "Available 9am-5pm on March 15" |
| **Recurring** | Weekly pattern | "Mon-Fri 8am-6pm" |
| **Recurring without weekend** | Skips Saturday/Sunday | "Mon-Fri 9am-5pm, closed weekends" |

```c
typedef enum {
    TP_WINDOW_CONTINUOUS,           /* One-time window */
    TP_WINDOW_RECURRING,            /* Weekly pattern */
    TP_WINDOW_RECURRING_NO_WEEKEND, /* Weekly, skip Sat/Sun */
} TPWindowType;

typedef struct {
    TPWindowType type;

    /* For CONTINUOUS */
    time_t earliest;
    time_t latest;

    /* For RECURRING */
    uint8_t days_of_week;       /* Bitmask: bit 0 = Sunday */
    int open_hour, open_min;
    int close_hour, close_min;

    /* Common */
    double max_allowed_delay;   /* Soft window extension */
} TPAppointment;
```

### Intermediate Tasks (ITSKs)

An intermediate task is an action inserted between main tasks, such as:
- Mandatory driver break at a specific location
- Home time visit
- Fixed appointment mid-route
- Fuel stop (when timing matters)

Each ITSK can define:

```c
typedef struct {
    int id;

    /* Time window (when itsk must be performed) */
    time_t earliest;
    time_t latest;
    int has_window;

    /* Duration requirements */
    double min_duration;        /* Minimum off-duty/service time */

    /* Location (optional) */
    int has_location;
    double lat, lon;
    double transit_to_duration; /* Drive time to itsk location */
    double transit_from_duration;/* Drive time from itsk to next task */

    /* Constraints */
    double max_transit_from;    /* Max time/distance from itsk to next task */
    int is_required;            /* Hard requirement vs optional */
} TPIntermediateTask;
```

**ITSK Placement Strategy:**

During search, ITSKs are tentatively placed between tasks:
- Try placing before each task in the sequence
- Verify ITSK time window is satisfied
- Verify transit-from constraint is not violated
- Select feasible placement that minimizes total duration

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

**Phase 1: Time Window Core**
- [ ] Define TPTimeWindow and TPAppointment structures
- [ ] Implement continuous window evaluation
- [ ] Implement recurring window evaluation (with/without weekends)
- [ ] Implement time window intersection/union operations
- [ ] Create test suite for window operations

**Phase 2: Constraint Types**
- [ ] Implement facility hours checking
- [ ] Implement blackout/exclusion window checking
- [ ] Implement max transit distance/duration constraints
- [ ] Implement service time requirements

**Phase 3: Intermediate Tasks (ITSKs)**
- [ ] Define ITSK data structure
- [ ] Implement ITSK time window validation
- [ ] Implement ITSK placement feasibility checking
- [ ] Implement max-transit-from constraint enforcement
- [ ] Create test suite for ITSK scenarios

**Phase 4: Constraint Evaluation Engine**
- [ ] Implement combined constraint evaluation
- [ ] Add soft constraint penalty calculation
- [ ] Implement delay tolerance handling

**Phase 5: Integration**
- [ ] Integrate with HoSE for combined feasibility
- [ ] Integrate with Pulse for schedule validation
- [ ] Add API endpoints for constraint management

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
- Use explicit stack, NOT recursion (better for deep searches)
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

### DFS Algorithm (Explicit Stack)

```
PROCEDURE TreeSearch(root, params):
    stack ← [root]
    solutions ← empty priority queue (by objective value, max size N)

    WHILE stack is not empty AND can_continue(params):
        node ← stack.pop()

        IF is_leaf(node):
            value ← evaluate_path(node)
            IF value is valid:
                solutions.insert(node, value)
        ELSE:
            children ← enumerate_children(node, params)
            children ← apply_selection(children, params)
            children ← apply_pruning(children, solutions, params)

            FOR each child in children (in reverse order for DFS):
                stack.push(child)

    RETURN solutions
```

### Child Enumeration

For each parent node:
1. Compute remaining time in planning horizon
2. Iterate candidate actions and intermediate task placements
3. For each candidate:
   - Compute **lower-bound** duration (fast filter)
   - If promising, compute **exact** duration (full validation)
   - Reject if exceeds time limit or violates hard constraints
4. Compute updated objective totals for each feasible child

```c
typedef struct {
    int *candidate_indices;     /* Which actions to try */
    int num_candidates;
    double *lower_bounds;       /* Fast feasibility filter */
    double remaining_horizon;   /* Time left in planning span */
} AREnumContext;
```

### Child Selection Heuristic

Selection ranks children and picks the top ones for expansion:

```c
typedef enum {
    AR_SELECT_MAX_VALUE,        /* By objective value */
    AR_SELECT_MAX_PROFIT,       /* By profit contribution */
    AR_SELECT_ROUND_ROBIN,      /* Alternate between strategies */
    AR_SELECT_MIN_COST,         /* By cost (for minimization) */
} ARSelectStrategy;

/* Round-robin: alternate between value and profit to diversify search */
```

If no feasible child is found, emit an **unassigned sentinel node** that marks this as a leaf.

### Pruning Heuristics (Composable)

Multiple pruning rules are applied in sequence:

**1. Max Branch Count (Branching Factor)**
- For depth `d`, keep only first `branching_factors[d]` children
- Returns PRUNE_ALL for remaining siblings once limit hit

```c
int branching_factors[] = {10, 8, 6, 4, 3, 2, 2, 2, ...};  /* Per-depth limits */
```

**2. Remaining Objective Upper Bound**
- Precompute best achievable objective rate per action
- For current node, estimate max possible remaining objective
- If `current + remaining + future < best_known`, prune

```c
/* Upper bound calculation */
double estimate_remaining(ARState *state, ARParams *params) {
    double remaining_time = params->horizon - state->elapsed;
    double best_rate = precomputed_max_rate;
    return remaining_time * best_rate;
}
```

**3. Forced Coverage (Override)**
- When certain actions are prioritized/committed, protect nodes on paths that include them
- Relax branch-count pruning to ensure at least one plan reaches each prioritized action
- Allow extra branches up to `max_extra_branches_per_prioritized` depth

### Stop Conditions

**Leaf Detection (node is complete):**
- Unassigned: no feasible children generated
- Max depth reached: `depth >= max_allowed_depth`
- Planning horizon reached: `elapsed >= max_total_duration`

**Global Search Stop:**
- Wall-clock time limit exceeded
- Node limit exceeded
- Solution count limit reached
- Gap tolerance achieved

```c
typedef struct {
    int max_nodes;
    double max_duration_ms;
    int max_solutions;
    double gap_tolerance;
} ARStopConditions;
```

### Solution Diversity

Maintain a **capped pool** of top-N solutions:
- New solutions replace worst if better
- Lower bound for pruning = worst solution in pool
- Prevents memory explosion on easy problems

```c
#define AR_MAX_SOLUTIONS 100

typedef struct {
    ARState *solutions[AR_MAX_SOLUTIONS];
    double objectives[AR_MAX_SOLUTIONS];
    int count;
} ARSolutionPool;
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

**Phase 1: Core Infrastructure**
- [ ] Define ARState and ARCallbacks interface
- [ ] Implement state pool with efficient allocation
- [ ] Implement explicit-stack DFS (not recursive)
- [ ] Add stop condition checking (time, nodes, solutions)

**Phase 2: Child Management**
- [ ] Implement child enumeration framework
- [ ] Implement lower-bound filtering for child pruning
- [ ] Implement exact feasibility checking
- [ ] Implement child selection strategies (value, profit, round-robin)

**Phase 3: Pruning Heuristics**
- [ ] Implement branching factor limits (per-depth)
- [ ] Implement remaining-objective upper bound pruning
- [ ] Implement prioritized/committed action protection
- [ ] Make pruning rules composable

**Phase 4: Solution Management**
- [ ] Implement solution pool with fixed capacity
- [ ] Implement solution insertion/eviction
- [ ] Use pool worst solution as lower bound for pruning
- [ ] Add solution callback mechanism

**Phase 5: Advanced Features**
- [ ] Implement best-first search with priority queue
- [ ] Implement beam search
- [ ] Implement warm start with incumbent
- [ ] Implement dominance-based pruning (optional)
- [ ] Add comprehensive search statistics

**Phase 6: Applications**
- [ ] Create trip planning example (integrate HoSE + Tempo)
- [ ] Create scheduling example
- [ ] Benchmark on realistic problem sizes

---

## 5. Sigma - Fleet Plan Selection Engine

**S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation

### Overview

A MIP-based engine that selects the optimal subset of candidate plans to cover all fleet requirements. Arbor generates many feasible individual trip/route plans; Sigma chooses which plans to execute to optimize fleet-wide objectives while ensuring all demands are met.

This is a classic **Set Covering / Set Partitioning** problem:
- Each candidate plan "covers" certain demands (pickups, deliveries, customers)
- Select minimum-cost subset of plans such that every demand is covered exactly once (partitioning) or at least once (covering)

### Problem Formulation

**Set Partitioning (each demand served exactly once):**
```
minimize:    Σⱼ cⱼ xⱼ                    (total cost of selected plans)
subject to:  Σⱼ aᵢⱼ xⱼ = 1    ∀i        (each demand i covered exactly once)
             xⱼ ∈ {0,1}                  (select or not)
```

**Set Covering (each demand served at least once):**
```
minimize:    Σⱼ cⱼ xⱼ
subject to:  Σⱼ aᵢⱼ xⱼ ≥ 1    ∀i        (each demand i covered at least once)
             xⱼ ∈ {0,1}
```

Where:
- j ∈ Plans (candidate plans from Arbor)
- i ∈ Demands (stops, customers, loads to serve)
- cⱼ = cost of plan j (fuel, time, driver cost, etc.)
- aᵢⱼ = 1 if plan j serves demand i, 0 otherwise

### Additional Constraints

Real fleet optimization needs more than basic set covering:

| Constraint Type | Description |
|----------------|-------------|
| **Vehicle availability** | Each vehicle can execute at most one plan |
| **Driver assignment** | Match drivers to compatible plans |
| **Fleet capacity** | Limit on total vehicles deployed |
| **Balance constraints** | Vehicles must return to depots |
| **Time coordination** | Plans with shared resources can't overlap |
| **Minimum utilization** | Don't select very short/inefficient plans |

**Extended formulation:**
```
minimize:    Σⱼ cⱼ xⱼ + Σᵥ fᵥ yᵥ         (plan costs + vehicle fixed costs)

subject to:  Σⱼ aᵢⱼ xⱼ = 1       ∀i      (demand coverage)
             Σⱼ∈Pᵥ xⱼ ≤ yᵥ      ∀v      (vehicle v used if any of its plans selected)
             Σᵥ yᵥ ≤ K                   (fleet size limit)
             xⱼ, yᵥ ∈ {0,1}
```

### High-Level Architecture

```
sigma/
├── include/
│   ├── sigma.h             # Public API
│   ├── sg_types.h          # Data structures
│   ├── sg_model.h          # MIP model building
│   └── sg_column.h         # Column (plan) management
├── src/
│   ├── sigma.c             # Main API implementation
│   ├── sg_model.c          # Build set covering MIP
│   ├── sg_solve.c          # Solve and extract solution
│   ├── sg_column.c         # Column pool management
│   └── sg_pricing.c        # Dynamic column generation (optional)
├── tests/
│   └── test_sigma.c        # Unit tests
└── CLAUDE.md
```

### Core Data Structures (Preliminary)

```c
// sg_types.h

/* A demand that must be covered */
typedef struct {
    int id;
    char description[64];
    double penalty;             /* Cost of not covering (for soft demands) */
    int is_required;            /* Hard vs soft demand */
} SGDemand;

/* A candidate plan (column in set covering) */
typedef struct {
    int id;
    double cost;                /* Objective coefficient */

    /* Which demands this plan covers */
    int *covered_demands;
    int num_covered;

    /* Resource usage */
    int vehicle_id;             /* Which vehicle (-1 if any) */
    int driver_id;              /* Which driver (-1 if any) */
    double duration;            /* Total plan duration */
    time_t start_time;
    time_t end_time;

    /* Link back to Arbor solution */
    void *arbor_state;          /* Original ARState from Arbor */
} SGPlan;

/* Fleet optimization problem */
typedef struct {
    SGDemand *demands;
    int num_demands;

    SGPlan *plans;
    int num_plans;
    int plans_capacity;

    /* Fleet resources */
    int num_vehicles;
    int num_drivers;
    double *vehicle_costs;      /* Fixed cost per vehicle used */

    /* Constraints */
    int max_vehicles;           /* Fleet size limit */
    int require_partition;      /* 1 = exactly once, 0 = at least once */
} SGProblem;

/* Solution: which plans to execute */
typedef struct {
    int *selected_plans;        /* Indices of selected plans */
    int num_selected;

    double total_cost;
    int *uncovered_demands;     /* Demands not covered (if soft) */
    int num_uncovered;

    /* Per-vehicle assignments */
    int *vehicle_plan;          /* vehicle_plan[v] = plan assigned to vehicle v */
} SGSolution;
```

### Core API (Preliminary)

```c
// sigma.h

/**
 * Create fleet optimization problem
 */
SGProblem* sg_create(int num_demands, int num_vehicles);

/**
 * Add a demand to be covered
 */
int sg_add_demand(SGProblem *problem, const char *description, int is_required);

/**
 * Add a candidate plan
 */
int sg_add_plan(SGProblem *problem, double cost,
                const int *covered_demands, int num_covered,
                int vehicle_id, int driver_id);

/**
 * Add plan directly from Arbor state
 */
int sg_add_plan_from_arbor(SGProblem *problem, const ARState *state,
                           double cost, const int *covered_demands, int num_covered);

/**
 * Solve the fleet optimization problem
 */
int sg_solve(SGProblem *problem, SGSolution *solution);

/**
 * Free resources
 */
void sg_free_problem(SGProblem *problem);
void sg_free_solution(SGSolution *solution);
```

### Integration with Ralph MIP Solver

```c
int sg_solve(SGProblem *problem, SGSolution *solution)
{
    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    // Variables: x[j] = 1 if plan j selected
    for (int j = 0; j < problem->num_plans; j++) {
        ralph_add_var(model, 0, 1, problem->plans[j].cost, RALPH_BINARY);
    }

    // Constraints: each demand covered (exactly once or at least once)
    for (int i = 0; i < problem->num_demands; i++) {
        // Find all plans covering demand i
        int *indices = /* plans covering demand i */;
        double *coeffs = /* all 1.0 */;
        int nnz = /* count */;

        RalphSense sense = problem->require_partition ? RALPH_EQUAL : RALPH_GREATER_EQUAL;
        ralph_add_constraint(model, nnz, indices, coeffs, sense, 1.0);
    }

    // Vehicle constraints: at most one plan per vehicle
    for (int v = 0; v < problem->num_vehicles; v++) {
        // Find all plans for vehicle v
        // Add constraint: sum <= 1
    }

    // Solve
    ralph_optimize(model);

    // Extract solution
    if (ralph_get_status(model) == RALPH_STATUS_OPTIMAL) {
        double *x = malloc(problem->num_plans * sizeof(double));
        ralph_get_solution(model, x);
        // Populate solution->selected_plans where x[j] > 0.5
    }

    ralph_free(model);
    return 0;
}
```

### Prioritized/Committed Loads

Some demands may be pre-committed (contracted loads) that **must** be covered:

```c
typedef struct {
    int demand_id;
    int is_committed;           /* Must be covered, no exception */
    int is_prioritized;         /* Should be covered if at all feasible */
    double priority_weight;     /* Higher = more important */
} SGDemandPriority;
```

**Handling in MIP**:

```
# Committed demands: hard constraint
Σⱼ aᵢⱼ xⱼ = 1     ∀i ∈ CommittedDemands

# Prioritized demands: soft constraint with penalty
Σⱼ aᵢⱼ xⱼ + slack[i] = 1     ∀i ∈ PrioritizedDemands
minimize: Σⱼ cⱼ xⱼ + Σᵢ penalty[i] * slack[i]

# Optional demands: covered if beneficial
Σⱼ aᵢⱼ xⱼ ≤ 1     ∀i ∈ OptionalDemands
```

### Plan End Conditions

Different business scenarios require different plan ending rules:

| Condition | Description | Use Case |
|-----------|-------------|----------|
| `LOADED_OR_EMPTY` | Plan can end loaded or empty | Flexible operations |
| `EMPTY_TRUCK` | Must complete all deliveries within horizon | Strict deadlines |
| `EMPTY_WITH_PICKUP_OPTION` | Must be empty, but can accept pickup beyond horizon | Next-day coverage |
| `FORCED_TAKEHOME` | Must end at home location | Work-life balance |

```c
typedef enum {
    SG_END_LOADED_OR_EMPTY,
    SG_END_EMPTY_TRUCK,
    SG_END_EMPTY_WITH_HIDDEN_PICKUP,  /* Pickup beyond horizon, not counted */
    SG_END_EMPTY_WITH_VISIBLE_PICKUP, /* Pickup beyond horizon, counted */
} SGPlanEndCondition;
```

### Advanced: Column Generation

For very large plan sets, use dynamic column generation (Dantzig-Wolfe):

1. Start with subset of plans (initial columns)
2. Solve LP relaxation of set covering
3. Get dual prices π for demand constraints
4. **Pricing subproblem**: Find new plan with negative reduced cost
   - reduced_cost = plan_cost - Σᵢ πᵢ × (plan covers demand i)
   - This is where Arbor generates new plans guided by duals!
5. Add promising plans, repeat until no negative reduced cost plans
6. Solve final MIP with all generated columns

```c
/* Arbor-Sigma integration for column generation */
typedef struct {
    SGProblem *sigma;
    ARContext *arbor;
    double *dual_prices;        /* From LP relaxation */
} ColumnGenContext;

/* Arbor objective becomes: plan_cost - Σ π[i] for covered demands */
double cg_pricing_objective(const ARState *state, const ColumnGenContext *ctx) {
    double reduced_cost = ar_get_cost(state);
    for (int i = 0; i < ctx->sigma->num_demands; i++) {
        if (state_covers_demand(state, i)) {
            reduced_cost -= ctx->dual_prices[i];
        }
    }
    return reduced_cost;
}
```

### Integration Points

| Component | Integration |
|-----------|-------------|
| **Arbor** | Generates candidate plans for Sigma to select from |
| **Ralph** | Solves the set covering MIP |
| **HoSE** | Plans must be HoS-feasible |
| **Tempo** | Plans must satisfy time windows |
| **FuelWise** | Plan costs include fuel optimization |

### Workflow

```
┌─────────────────────────────────────────────────────────────┐
│                    Fleet Optimization Pipeline               │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  1. Input: Demands (loads, stops, customers)                 │
│            Fleet (vehicles, drivers, depots)                 │
│            Constraints (HoS, time windows, capacity)         │
│                         ↓                                    │
│  2. Arbor: Generate candidate plans per vehicle              │
│            - State-space search with HoSE/Tempo constraints  │
│            - Produces 10-100+ feasible plans per vehicle     │
│                         ↓                                    │
│  3. Sigma: Select optimal plan combination                   │
│            - Build set covering MIP                          │
│            - Solve with Ralph                                │
│            - Extract: which plans, which vehicles            │
│                         ↓                                    │
│  4. Output: Fleet schedule                                   │
│             - Vehicle → Plan assignments                     │
│             - Driver → Vehicle assignments                   │
│             - Complete timeline with stops, breaks, fuel     │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### TODOs

**Phase 1: Core Data Structures**
- [ ] Define SGDemand, SGPlan, SGSolution structures
- [ ] Define demand priority levels (committed, prioritized, optional)
- [ ] Define plan end conditions

**Phase 2: MIP Model Building**
- [ ] Implement basic set covering constraint generation
- [ ] Implement set partitioning variant
- [ ] Implement vehicle assignment constraints (one plan per vehicle)
- [ ] Implement fleet size limit constraint

**Phase 3: Advanced Constraints**
- [ ] Implement prioritized/committed demand handling
- [ ] Implement soft demand penalties
- [ ] Implement driver-vehicle compatibility constraints

**Phase 4: Ralph Integration**
- [ ] Integrate with Ralph MIP solver
- [ ] Implement solution extraction and validation
- [ ] Add plan import from Arbor states

**Phase 5: Column Generation (Advanced)**
- [ ] Implement LP relaxation solving
- [ ] Implement dual price extraction
- [ ] Implement pricing subproblem interface (for Arbor)
- [ ] Implement column pool management
- [ ] Implement iterative column generation loop

**Phase 6: Testing and Benchmarking**
- [ ] Create test suite with small fleet scenarios
- [ ] Benchmark on medium-scale problems (100 demands, 1000 plans)
- [ ] Test column generation on large-scale problems

---

## 6. Pulse - Execution Tracker and PTA Engine

**P**lan **U**tilization and **L**ive **S**tate **E**stimator

### Overview

Pulse is an execution tracking engine that takes a driver's current state and a sequence of assigned tasks, then simulates forward through time to compute:

1. **Predicted Time of Arrival (PTA)** at each stop
2. **Predicted Driver State** at each point (HoS clocks, status)
3. **Scheduled Actions** (drives, breaks, rest, service times)
4. **Feasibility Alerts** (will we violate HoS? miss time windows?)

Pulse is the "what happens if we execute this plan?" simulator, answering questions like:
- When will the driver arrive at stop #3?
- What will the driver's remaining driving hours be after delivery?
- Where should the driver take their 30-minute break?

### Key Distinction from Other Components

| Component | Role |
|-----------|------|
| **Arbor** | Explores *possible* plans (search) |
| **Sigma** | Selects *which* plans to execute (optimization) |
| **Pulse** | Computes *what happens* when executing a plan (simulation) |
| **HoSE** | Answers "can I drive X hours?" (rules) |
| **Tempo** | Answers "is this time window satisfied?" (constraints) |

Pulse *orchestrates* HoSE and Tempo to simulate plan execution.

### High-Level Architecture

```
pulse/
├── include/
│   ├── pulse.h             # Public API
│   ├── pl_types.h          # Data structures
│   ├── pl_task.h           # Task definitions
│   ├── pl_schedule.h       # Schedule building
│   └── pl_simulate.h       # Simulation engine
├── src/
│   ├── pulse.c             # Main API implementation
│   ├── pl_simulate.c       # Forward simulation
│   ├── pl_schedule.c       # Break/rest scheduling
│   ├── pl_optimize.c       # Optimal break placement (optional)
│   └── pl_report.c         # PTA reporting
├── tests/
│   └── test_pulse.c        # Unit tests
└── CLAUDE.md
```

### Core Data Structures (Preliminary)

```c
// pl_types.h

/* Task types */
typedef enum {
    PL_TASK_DRIVE,              /* Drive from A to B */
    PL_TASK_PICKUP,             /* Pickup at location */
    PL_TASK_DELIVERY,           /* Delivery at location */
    PL_TASK_FUEL,               /* Refueling stop */
    PL_TASK_WAIT,               /* Wait (for time window) */
    PL_TASK_BREAK,              /* HoS break */
    PL_TASK_REST,               /* HoS rest period */
} PLTaskType;

/* A task in the plan */
typedef struct {
    PLTaskType type;
    int location_id;            /* Where (for non-drive tasks) */
    int origin_id;              /* For drive: starting point */
    int destination_id;         /* For drive: ending point */

    double drive_duration;      /* Driving time (for DRIVE tasks) */
    double drive_distance;      /* Distance (for DRIVE tasks) */
    double service_duration;    /* Service time (pickup/delivery/fuel) */

    /* Time window (from Tempo) */
    time_t window_start;
    time_t window_end;
    int has_window;
} PLTask;

/* A plan: sequence of tasks to execute */
typedef struct {
    PLTask *tasks;
    int num_tasks;

    int driver_id;
    int vehicle_id;

    time_t plan_start;          /* When plan execution begins */
} PLPlan;

/* Scheduled action in the timeline */
typedef struct {
    PLTaskType type;
    int task_index;             /* Which task this action is for (-1 for inserted breaks) */

    time_t start_time;
    time_t end_time;
    double duration;

    int location_id;
    char description[128];

    /* Driver state at end of this action */
    HSDriverState driver_state_after;
} PLAction;

/* Complete execution schedule */
typedef struct {
    PLAction *actions;
    int num_actions;

    /* PTAs for each task */
    time_t *task_pta;           /* task_pta[i] = predicted arrival for task i */
    HSDriverState *task_state;  /* driver state when starting task i */

    /* Summary */
    time_t plan_end_time;
    double total_drive_time;
    double total_wait_time;
    double total_break_time;
    double total_rest_time;

    /* Feasibility */
    int is_feasible;
    int num_warnings;
    char **warnings;            /* "Will miss window at task 3", etc. */
} PLSchedule;
```

### Core API (Preliminary)

```c
// pulse.h

/**
 * Simulate plan execution and compute PTA schedule
 *
 * @param plan          The plan to simulate
 * @param initial_state Driver's HoS state at plan start
 * @param schedule      Output: computed schedule with PTAs
 * @return              0 on success, -1 on error
 */
int pl_simulate(const PLPlan *plan, const HSDriverState *initial_state,
                PLSchedule *schedule);

/**
 * Simulate with automatic break insertion
 * Uses HoSE to determine when breaks are needed
 * Optionally optimizes break placement with Ralph LP
 */
int pl_simulate_with_breaks(const PLPlan *plan, const HSDriverState *initial_state,
                            int optimize_breaks, PLSchedule *schedule);

/**
 * Get PTA for a specific task
 */
time_t pl_get_pta(const PLSchedule *schedule, int task_index);

/**
 * Get driver state at a specific task
 */
HSDriverState pl_get_state_at_task(const PLSchedule *schedule, int task_index);

/**
 * Check if plan will violate any time windows
 */
int pl_check_windows(const PLSchedule *schedule, int *violations, int *num_violations);

/**
 * Free schedule resources
 */
void pl_free_schedule(PLSchedule *schedule);
```

### Simulation Algorithm

```
simulate(plan, initial_state):
    current_time = plan.start_time
    current_state = copy(initial_state)
    actions = []

    for each task in plan.tasks:
        # Record PTA for this task
        task_pta[task] = current_time

        if task.type == DRIVE:
            # Check if we can drive the full duration
            remaining_drive = task.drive_duration

            while remaining_drive > 0:
                # How long can we drive before HoS limit?
                max_drive = hs_max_driving_time(current_state)

                if max_drive <= 0:
                    # Insert required break/rest
                    required_break = hs_get_required_break(current_state)
                    actions.append(Action(BREAK/REST, current_time, required_break.duration))
                    hs_apply_action(current_state, required_break)
                    current_time += required_break.duration
                else:
                    drive_now = min(remaining_drive, max_drive)
                    actions.append(Action(DRIVE, current_time, drive_now))
                    hs_apply_action(current_state, drive_action)
                    current_time += drive_now
                    remaining_drive -= drive_now

        elif task.type in [PICKUP, DELIVERY, FUEL]:
            # Check time window
            if task.has_window and current_time < task.window_start:
                # Wait for window
                wait_duration = task.window_start - current_time
                actions.append(Action(WAIT, current_time, wait_duration))
                current_time = task.window_start

            if task.has_window and current_time > task.window_end:
                # Window violation!
                add_warning("Missed window at task %d", task)

            # Service time
            actions.append(Action(task.type, current_time, task.service_duration))
            current_time += task.service_duration

    return Schedule(actions, task_pta, ...)
```

### Break Placement Optimization

**Naive Strategy**: Insert breaks when forced by HoS limits.

**DP-Based Optimal Strategy**: Use dynamic programming to find optimal ITSK-to-segment assignment:

```
PROCEDURE OptimalSchedule(segments, itsks, driver_state):
    # DP state: (n_itsk_scheduled, n_segment_completed) → (state, duration, delay)
    dp[0][0] = (driver_state, 0, 0)

    FOR s = 0 TO num_segments - 1:
        FOR i = 0 TO num_itsks:
            IF dp[i][s] is valid:
                # Try assigning k itsks (k = 0..num_itsks-i) to segment s
                FOR k = 0 TO num_itsks - i:
                    new_state = simulate_segment_with_itsks(
                        dp[i][s].state,
                        segments[s],
                        itsks[i:i+k]
                    )
                    IF new_state is feasible:
                        objective = compute_objective(new_state, params)
                        IF objective < dp[i+k][s+1].objective:
                            dp[i+k][s+1] = new_state

    # Traceback from best terminal state
    RETURN reconstruct_schedule(dp)
```

**Scheduling Objectives**:

```c
typedef enum {
    PL_OBJ_MIN_DURATION,            /* Minimize total transit time */
    PL_OBJ_MIN_DURATION_NO_DELAY,   /* Minimize duration, reject any delay */
    PL_OBJ_MIN_TOTAL_DELAY,         /* Minimize sum of appointment delays */
    PL_OBJ_MIN_MAX_DELAY,           /* Minimize worst-case delay */
} PLScheduleObjective;
```

**LP-Based Break Optimization** (alternative to DP):

For simpler cases without ITSKs, formulate as MIP:

```
minimize:    Σ wait_time[i]

subject to:  # Arrival time at each stop
             arrival[i+1] = arrival[i] + drive[i] + service[i] + break[i] + wait[i]

             # Can't arrive before window opens
             arrival[i] >= window_start[i] - wait[i]

             # HoS: driving since last break ≤ 8 hours
             Σ drive[j] for j since last break ≤ 8 * 3600

             # Break placement (binary decision)
             break[i] ≥ 0
             break[i] ≤ M * has_break[i]  (has_break is binary)
```

This can be formulated as a small MIP and solved with Ralph.

### Schedule Result Structure

```c
typedef struct {
    /* Per-segment data */
    int *itsk_assignment;           /* itsk_assignment[i] = segment for itsk i */
    HSDriverState *state_before;    /* Driver state before each segment */
    HSDriverState *state_after;     /* Driver state after each segment */

    /* Action timeline */
    PLAction *actions;              /* Flattened list of all actions */
    int num_actions;
    int *action_is_itsk;            /* Which actions correspond to ITSKs */
    int *action_is_inspection;      /* Which actions are pre/post inspections */

    /* Scheduling result */
    enum {
        PL_SCHEDULING_OPTIMAL,      /* All segments scheduled */
        PL_SCHEDULING_PARTIAL,      /* Only prefix schedulable */
        PL_SCHEDULING_INFEASIBLE,   /* No feasible schedule */
    } status;

    /* Summary metrics */
    double total_duration;
    double total_delay;
    double max_delay;
} PLScheduleResult;
```

### Integration Points

| Component | Integration |
|-----------|-------------|
| **HoSE** | Provides driving limits and break requirements |
| **Tempo** | Provides time window constraints for tasks |
| **Velo** | Provides drive durations between locations |
| **FuelWise** | Provides fuel stop locations and durations |
| **Ralph** | Optionally optimizes break placement |

### Use Cases

1. **Dispatch Planning**: "If driver starts now, when will they arrive at each stop?"
2. **What-If Analysis**: "What if we add this pickup to the route?"
3. **ETA Updates**: "Driver is at stop 2, update ETAs for remaining stops"
4. **Compliance Check**: "Will this plan cause any HoS violations?"
5. **Schedule Display**: "Show the driver their break schedule"

### Workflow Integration

```
┌─────────────────────────────────────────────────────────────┐
│                    End-to-End Pipeline                       │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  Arbor: Generate candidate plans                             │
│                         ↓                                    │
│  Pulse: Simulate each plan → compute PTAs, check feasibility │
│                         ↓                                    │
│  Sigma: Select best plans (with accurate costs from Pulse)   │
│                         ↓                                    │
│  Pulse: Generate final schedules for selected plans          │
│                         ↓                                    │
│  Output: Driver schedules with PTAs and break times          │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

### TODOs

**Phase 1: Core Data Structures**
- [ ] Define PLTask, PLPlan, PLAction structures
- [ ] Define PLSchedule and PLScheduleResult structures
- [ ] Define scheduling objective enum

**Phase 2: Basic Simulation**
- [ ] Implement forward simulation loop
- [ ] Integrate HoSE for driving limits
- [ ] Implement automatic break insertion (naive)
- [ ] Compute PTA for each task

**Phase 3: Time Window Handling**
- [ ] Integrate Tempo for window checking
- [ ] Implement wait time computation for early arrivals
- [ ] Implement window violation detection
- [ ] Generate warnings for missed windows

**Phase 4: ITSK Handling**
- [ ] Implement ITSK-aware simulation
- [ ] Implement DP-based optimal ITSK assignment
- [ ] Support multiple scheduling objectives

**Phase 5: Optimization**
- [ ] Implement LP-based break placement (Ralph integration)
- [ ] Implement slack merging optimization
- [ ] Benchmark simulation performance

**Phase 6: Reporting and Integration**
- [ ] Create schedule visualization/reporting
- [ ] Implement driver state snapshots at each task
- [ ] Create action-level audit trail
- [ ] Test with realistic multi-stop routes

---

---

## 7. Distance and Duration Estimation (Cross-Cutting)

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

## 8. Cost and Profit Calculations (Cross-Cutting)

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

## 9. FuelWise Integration (Refueling in Search)

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
| `api/` | Active | REST API server |
| `ui/` | Active | React frontend |

## Implementation Priority

1. **Project Renaming to OTTO** - Low priority (cosmetic, do when convenient)
2. **HoSE Core** - High priority (essential for realistic trucking optimization)
3. **Tempo Core** - High priority (time windows needed for realistic planning)
4. **Pulse Core** - High priority (PTA computation needed everywhere)
5. **Arbor Core** - Medium priority (enables advanced optimization)
6. **Sigma Core** - Medium priority (fleet-wide optimization, depends on Arbor)
7. **Component Integration** - High priority (HoSE + Tempo + Pulse + Arbor + Sigma + FuelWise)
