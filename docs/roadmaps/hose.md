# HoSE - Hours of Service Engine

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

