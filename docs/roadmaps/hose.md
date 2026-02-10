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

### LP/MIP Formulation for Verification & Benchmarking

The greedy heuristic ("drive when you can, rest when you must, start on-duty as late as possible to conserve the 14h window") needs verification against an optimal solution. This section defines MIP formulations that compute the **minimum span** schedule for a task or chain of tasks.

**Use case:** Verification tool to find corner cases in the heuristic, not production solving.

#### Problem Definition

**Given:**
- Driver state at t_now: (drive_11h_used, window_14h_used, break_8h_used, cycle_70h_used, daily_log[8])
- Task: t_drive (driving time), t_work (on-duty work at destination)
- Time window: [earliest_arrival, latest_departure]
  - Continuous: site open for entire interval
  - Recurring: site open daily (e.g., 10:00-17:00 Mon-Fri)

**Find:**
- Schedule minimizing **span** = t_finish - t_now
- Secondary objective: maximize remaining clock optionality (11h, 14h, 70h remaining)

**Subject to:**
- 11h driving limit (resets after 10h off-duty)
- 14h window (wall clock from first on-duty, resets after 10h off-duty)
- 8h driving triggers mandatory 30-min break
- 70h/8d or 60h/7d cycle with daily recap (resets after 34h off-duty)

**Definitions:**
- **Slack**: Arriving before earliest_arrival → off-duty waiting
- **Work**: On-duty non-driving at site (consumes 14h window and 70h, not 11h or 8h-break)
- **Optionality**: Preserving clocks = delaying on-duty start to not "waste" the 14h window

---

#### Formulation 1: Time-Indexed MIP

**Discretization:** δ = 1/12 hours (5 minutes), horizon H periods (1 week = 2016 periods)

##### Variables

```
x[t,s] ∈ {0,1}     State at period t: s ∈ {DRIVE, WORK, OFF}
d[t] ∈ [0,11]      11h clock: driving since last 10h reset
w[t] ∈ [0,14]      14h window: wall-clock elapsed since duty period started
b[t] ∈ [0,8]       8h break clock: driving since last 30-min break
c[t] ∈ [0,70]      70h cycle clock (or 60h for 60/7 mode)
in_duty[t] ∈ {0,1} Currently in active duty period (14h window running)
off_consec[t] ∈ ℤ+ Consecutive OFF periods ending at t
arrived[t] ∈ {0,1} Has completed driving (arrived at destination)
done[t] ∈ {0,1}    Task complete (work finished)
```

##### Initial State (Driver Already Mid-Activity)

The driver at t_now may already be in the middle of an activity:

**Input parameters:**
```
status_init ∈ {DRIVE, WORK, OFF}   Current activity at t_now
status_duration ∈ ℝ+               Time already spent in current activity (hours)
d0, w0, b0, c0                     Clock values at t_now
in_duty_init ∈ {0,1}               Is driver in an active duty period?
daily_log[1..8]                    On-duty hours for past 8 days (for recap)
```

**Why this matters:**
- If driver has been OFF for 7h, only 3h more needed for 10h reset
- If driver has been OFF for 25 min, only 5 min more for qualifying 30-min break
- If driver is OFF but `in_duty_init = 1`, the 14h window is still ticking (wall clock)
- If driver is OFF and `in_duty_init = 0`, they completed a 10h reset and window is paused

**Example scenarios:**

| Scenario | status_init | status_duration | in_duty_init | Effect |
|----------|-------------|-----------------|--------------|--------|
| Fresh driver (10h+ off) | OFF | 10+ hours | 0 | All clocks reset, ready to start new duty period |
| Mid-shift break | OFF | 1 hour | 1 | 14h window still running, need 9h more for reset |
| Driving continuously | DRIVE | 3 hours | 1 | Continue driving, clocks reflect 3h usage |
| Loading at dock | WORK | 2 hours | 1 | On-duty non-driving, 14h and 70h affected |
| Mid-34h restart | OFF | 20 hours | 0 | 14h more for 34h restart (70h reset) |

##### Constraints

**State exclusivity:**
```
∑_s x[t,s] = 1   ∀t
```

**Initial state from ongoing activity:**
```
// Initialize consecutive OFF counter based on current status
off_consec[0] = floor(status_duration / δ)  if status_init = OFF
off_consec[0] = 0                            otherwise

// Initialize in_duty based on whether driver is in active duty period
in_duty[0] = in_duty_init

// If driver continues same activity in period 0, streak continues
// If driver switches activity, streak resets appropriately
```

**Virtual "pre-history" for break detection:**

For the 30-min break rule, we need to know if the driver is already mid-break:
```
// Pre-history: status_init = OFF and status_duration ≥ k·δ means
// x[-k,OFF] = x[-k+1,OFF] = ... = x[-1,OFF] = 1 (virtually)

// Periods needed for 30-min break
break_periods = ceil(0.5 / δ)  // e.g., 6 periods at δ=5min

// Already accumulated OFF periods before t=0
pre_off = floor(status_duration / δ)  if status_init = OFF, else 0

// break30[t] triggers when total consecutive OFF ≥ break_periods
// For early periods (t < break_periods), include pre-history:
break30[t] = 1 iff (pre_off + consecutive OFF from 0 to t) ≥ break_periods
```

**11h clock dynamics:**
```
d[t] = d[t-1] + δ·x[t,DRIVE] - 11·reset10[t]
d[0] = d0  (initial state, reflects driving already done this shift)
x[t,DRIVE] ≤ (11 - d[t-1]) / δ    // Can't drive if clock exhausted
```

**14h window (wall clock from first on-duty):**
```
// Duty period state propagation
in_duty[t] ≥ in_duty[t-1] - reset10[t]           // stays 1 until reset
in_duty[t] ≥ x[t,DRIVE] + x[t,WORK]              // becomes 1 on activity
in_duty[t] ≤ in_duty[t-1] + x[t,DRIVE] + x[t,WORK]  // only starts on activity

// Initial duty state from input
in_duty[0] ≥ in_duty_init                         // preserve ongoing duty period
in_duty[0] ≥ x[0,DRIVE] + x[0,WORK]              // or start new one

// Window clock runs whenever in_duty = 1
w[t] = w[t-1]·(1 - reset10[t]) + δ·in_duty[t]
w[0] = w0  (initial state, may be mid-window)

// Can't drive if window exhausted
x[t,DRIVE] ≤ M·(1 - in_duty[t-1]) + (14 - w[t-1]) / δ
```

**30-minute break rule (with pre-history):**

Per FMCSA 2020 final rule:
- Break required after **8 cumulative hours of driving** (not on-duty)
- Both OFF and WORK (on-duty/not-driving) qualify as break time

```
b[t] = b[t-1]·(1 - break30[t]) + δ·x[t,DRIVE]
b[0] = b0  (initial state)

// Break qualification: 30 min of either OFF or WORK (on-duty/not-driving)
// Define: not_driving[t] = x[t,OFF] + x[t,WORK]
not_driving[t] = 1 - x[t,DRIVE]

// For periods t < break_periods, include pre-history in break detection
// pre_off = floor(status_duration / δ) if status_init ∈ {OFF, WORK}, else 0

// At t=0: if pre_off ≥ break_periods, driver already has qualifying break
// break30[0] = 1 iff pre_off ≥ break_periods

// For t ∈ [1, break_periods-1]:
// Need (pre_off + t+1) consecutive non-driving periods
// break30[t] = 1 iff not_driving[0..t] all 1 AND pre_off + t + 1 ≥ break_periods

// For t ≥ break_periods: standard rule (no pre-history needed)
break30[t] = 1 iff not_driving[t-break_periods+1..t] all 1

// Equivalently: break30[t] = 1 iff x[τ,DRIVE] = 0 for all τ ∈ [t-5, t] (at δ=5min)

// Can't drive if 8h break clock exhausted without qualifying break
x[t,DRIVE] ≤ (8 - b[t-1]) / δ + M·break30_available[t]
```

**10h and 34h reset detection (with pre-history):**
```
// Consecutive OFF counter (initialized from status_duration)
off_consec[t] = (off_consec[t-1] + 1)·x[t,OFF]
off_consec[0] = (pre_off + 1)·x[0,OFF]  // continue streak if still OFF

// Reset thresholds
reset10_periods = ceil(10 / δ)  // 120 at δ=5min
reset34_periods = ceil(34 / δ)  // 408 at δ=5min

// 10h reset triggers when consecutive OFF reaches threshold
reset10[t] = 1 iff off_consec[t] ≥ reset10_periods

// 34h reset triggers when consecutive OFF reaches threshold
reset34[t] = 1 iff off_consec[t] ≥ reset34_periods

// Edge case: if pre_off already ≥ threshold, reset happens at t=0
// (driver was already past the reset point when optimization starts)
```

**70h cycle with daily recap:**
```
// At each midnight, gain back hours from 8 days ago
recap[t] = daily_log[8] if t crosses midnight, else 0
c[t] = c[t-1] + δ·(x[t,DRIVE] + x[t,WORK]) - recap[t] - 70·reset34[t]
c[0] = c0

// Can't work or drive if cycle exhausted
x[t,DRIVE] + x[t,WORK] ≤ (70 - c[t-1]) / δ
```

**Task sequencing:**
```
// Accumulate driving and work
drive_done[t] = drive_done[t-1] + δ·x[t,DRIVE]
work_done[t] = work_done[t-1] + δ·x[t,WORK]

// Must complete required driving
drive_done[H] ≥ t_drive

// Must complete required work
work_done[H] ≥ t_work

// Can only work after arriving (driving complete)
x[t,WORK] ≤ arrived[t]
arrived[t] = 1 iff drive_done[t] ≥ t_drive
```

**Time windows:**
```
// Continuous [E, L]: work only within window
x[t,WORK] = 0  if t·δ + t_now < E
work_done[t] ≤ t_work  if t·δ + t_now > L

// Recurring (e.g., 10:00-17:00 daily):
x[t,WORK] = 0  if hour_of_day(t·δ + t_now) ∉ [10, 17]
```

**Slack handling:**
```
// After arriving, if before earliest_arrival, must be OFF (waiting)
arrived[t] = 1 ∧ (t·δ + t_now < E) → x[t,OFF] = 1
```

##### Objective

```
// Primary: minimize span
min span where done[span/δ] = 1

// Equivalent linearization:
min ∑_t δ·(1 - done[t])

// Secondary (lexicographic or weighted):
// Maximize remaining optionality
+ ε·(11 - d[T]) + ε·(14 - w[T]) + ε·(70 - c[T])
```

##### FMCSA 2020 Final Rule Compliance

The time-indexed MIP models the core HoS rules. Here's the status of the June 2020 provisions:

| Provision | Status | Notes |
|-----------|--------|-------|
| **11-Hour Driving Limit** | ✅ Modeled | `d[t] ≤ 11`, resets after 10h OFF |
| **14-Hour Driving Window** | ✅ Modeled | `w[t] ≤ 14`, wall-clock from first on-duty |
| **30-Minute Break** | ✅ Modeled | After 8h driving; OFF or WORK qualifies (2020 rule) |
| **60/70-Hour Limit** | ✅ Modeled | `c[t] ≤ 70` with daily recap |
| **34-Hour Restart** | ✅ Modeled | `reset34[t]` clears 70h clock |
| **Short-Haul Exception** | ⚪ Out of scope | Different ruleset for <150 air-mile operations |
| **Adverse Driving Conditions** | ⚠️ Not modeled | See extension below |
| **Sleeper Berth Split** | ⚠️ Not modeled | See extension below |

**Extension: Adverse Driving Conditions**

When adverse conditions occur, driver may extend both 11h and 14h limits by up to 2h:

```
// Input parameter
adverse_conditions ∈ {0,1}   // 1 if adverse conditions apply
adverse_extension = 2.0      // hours (max allowed)

// Modified limits
driving_limit = 11 + adverse_extension·adverse_conditions  // 11 or 13
window_limit = 14 + adverse_extension·adverse_conditions   // 14 or 16

// Updated constraints
x[t,DRIVE] ≤ (driving_limit - d[t-1]) / δ
x[t,DRIVE] ≤ M·(1 - in_duty[t-1]) + (window_limit - w[t-1]) / δ
```

**Extension: Sleeper Berth Split**

The 2020 rule allows 7+2 or 7+3 splits where neither period counts against the 14h window:

```
// Additional state: s ∈ {DRIVE, WORK, OFF, SLEEPER}
x[t,s] ∈ {0,1}   for s ∈ {DRIVE, WORK, OFF, SLEEPER}

// Track sleeper berth periods
sleeper_consec[t] = (sleeper_consec[t-1] + 1)·x[t,SLEEPER]

// Detect qualifying split (7h sleeper completed)
split_7h[t] = 1 iff sleeper_consec[t] ≥ 7/δ

// After 7h sleeper, driver needs 2-3h more OFF (outside berth) to complete split
// Track off_after_sleeper[t] for consecutive OFF after split_7h

// Key insight: during a valid split, the 14h window PAUSES
// w[t] only advances when in_duty[t] = 1 AND NOT in a valid split period

// Simplified: track split_active[t] = 1 during split rest periods
// w[t] = w[t-1]·(1 - reset10[t]) + δ·in_duty[t]·(1 - split_active[t])
```

The sleeper berth logic is complex and adds significant model size. For verification purposes, consider:
1. Testing non-split scenarios first (most common)
2. Adding split support as a separate model variant
3. Using the heuristic for split decisions, MIP for verification

---

#### Formulation 2: Event-Based MIP

Model the schedule as a sequence of up to N activity segments.

##### Variables

```
s[i]               Start time of segment i (continuous)
e[i]               End time of segment i (e[i] = s[i+1])
dur[i] = e[i] - s[i]
type[i] ∈ {DRIVE, WORK, OFF}  (binary encoding)

// Clock states at end of segment i
d[i] ∈ [0,11]      11h clock after segment i
w[i] ∈ [0,14]      14h window after segment i
b[i] ∈ [0,8]       8h break clock after segment i
c[i] ∈ [0,70]      70h clock after segment i

// Reset/break indicators
is_reset10[i] ∈ {0,1}   Segment i is OFF with dur[i] ≥ 10h
is_reset34[i] ∈ {0,1}   Segment i is OFF with dur[i] ≥ 34h
is_break30[i] ∈ {0,1}   Segment i is non-driving (OFF or WORK) with dur[i] ≥ 0.5h
```

##### Constraints

**Segment ordering:**
```
e[i] = s[i+1]   ∀i
s[0] = t_now
dur[i] ≥ 0
```

**11h clock (big-M linearization):**
```
d[i] ≥ d[i-1] + dur[i] - M·(1 - isDRIVE[i]) - 11·is_reset10[i]
d[i] ≤ d[i-1] + dur[i] + M·(1 - isDRIVE[i])
d[i] ≤ 11
isDRIVE[i] = 1 → d[i-1] + dur[i] ≤ 11  // can't exceed while driving
```

**14h window (requires tracking duty period start):**
```
// duty_start[i] = time when current duty period started
// Complex: must track across segments using big-M

// Simpler approach: track in_duty[i] and w[i]
in_duty[i] = in_duty[i-1]·(1 - is_reset10[i-1]) + (isDRIVE[i] + isWORK[i])·(1 - in_duty[i-1])

// Window accumulates wall-clock time while in_duty
w[i] = (w[i-1] + dur[i])·in_duty[i]·(1 - is_reset10[i])  // nonlinear, needs linearization

isDRIVE[i] = 1 → w[i] ≤ 14
```

**30-min break (2020 rule: OFF or WORK qualifies):**
```
// Non-driving segment (OFF or WORK) can satisfy break requirement
is_break30[i] ≤ isOFF[i] + isWORK[i]   // must be non-driving
is_break30[i] ≤ 1 - isDRIVE[i]          // equivalent
is_break30[i] → dur[i] ≥ 0.5

b[i] = b[i-1]·(1 - is_break30[i]) + dur[i]·isDRIVE[i]
isDRIVE[i] = 1 → b[i] ≤ 8
```

**Reset detection:**
```
is_reset10[i] ≤ isOFF[i]
is_reset10[i] → dur[i] ≥ 10

is_reset34[i] ≤ is_reset10[i]
is_reset34[i] → dur[i] ≥ 34
```

**Task completion:**
```
∑_i dur[i]·isDRIVE[i] ≥ t_drive
∑_i dur[i]·isWORK[i] ≥ t_work

// All WORK segments after all DRIVE segments
```

##### Objective

```
min span = e[last] - s[0]
```

---

#### Comparison: Time-Indexed vs Event-Based

| Aspect | Time-Indexed | Event-Based |
|--------|--------------|-------------|
| **Model size** | O(H/δ) variables; 1 week at 5-min = 2016 periods | O(N) segments; typically N ≤ 20 |
| **14h window** | Easy: just sum `in_duty[t]·δ` | Hard: requires tracking duty period start with big-M |
| **30-min break** | Easy: count consecutive OFF periods | Moderate: detect qualifying breaks |
| **Recurring windows** | Easy: mask invalid periods | Hard: split work across intervals |
| **Recap (70h rollover)** | Easy: trigger at midnight periods | Moderate: detect midnight crossings |
| **Precision** | Quantized to δ | Exact continuous time |
| **Solve time** | Slower (more variables), but simpler | Faster if well-formulated, but big-M issues |
| **Solution interpretation** | Direct: read x[t,s] matrix | Abstract: sequence of segments |
| **Debugging** | Easy: visualize period-by-period | Harder: must trace constraint logic |

**Recommendation:** Use **time-indexed** for verification because:
1. The 14h window is much easier to model correctly
2. Recurring time windows are trivial
3. Solution is easy to visualize and verify
4. 5-minute granularity is fine for trucking (HoS rules don't care about seconds)

---

#### Feasibility Analysis

**1 week horizon at δ = 5 minutes:**
- Periods: 7 × 24 × 12 = **2016 periods**
- Variables: ~6000 binary (x[t,s]), ~8000 continuous (clocks)
- Constraints: ~15000

**Solve time estimate (Ralph):**
- Single task: **1-5 seconds** (depends on problem structure)
- Chain of 5 tasks: **5-30 seconds**

**Memory:** ~50-100 MB for the model

This is tractable for verification/benchmarking. Not suitable for real-time production, which is fine since the heuristic handles that.

---

#### Performance Considerations

##### MIP Structure

The time-indexed formulation is a **MIP** (Mixed Integer Program):
- Binary: `x[t,s]` state variables (~6000), reset/break indicators, completion flags
- Continuous: clock states `d[t]`, `w[t]`, `b[t]`, `c[t]`

**Why it solves fast despite size:**
1. **Path structure**: States form a sequence through time—once in a state, you transition or stay
2. **Tight LP relaxation**: Continuous relaxation is often near-integral
3. **Natural branching**: Branch on "when does first break occur?" converges quickly
4. **Sparse constraints**: Each period only links to adjacent periods

Most instances solve at the root node with presolve + cuts. Branching is rare.

##### Handling Non-Aligned Times

Real inputs don't align to δ boundaries:
- Driving time: 2h 37min (not a multiple of 5 min)
- Loading time: 45 min (aligns) or 47 min (doesn't)
- Time window: [10:23, 17:45] (odd boundaries)

**Approach: Continuous accumulators with period triggers**

```
# Continuous variables track exact totals
drive_total[t] ∈ ℝ       # Actual driving time accumulated
work_total[t] ∈ ℝ        # Actual work time accumulated
arrival_time ∈ ℝ         # Exact arrival time (continuous)

# Period-based updates (each active period contributes δ)
drive_total[t] = drive_total[t-1] + δ·x[t,DRIVE]
work_total[t] = work_total[t-1] + δ·x[t,WORK]

# Completion: exact threshold (not quantized)
drive_total[H] ≥ t_drive    # e.g., 2.617 hours exactly

# Arrival time: first period where driving is complete
arrival_time ≥ t·δ - M·(1 - arrived[t])   ∀t
arrival_time ≤ t·δ + M·arrived[t]         ∀t
arrived[t] = 1 iff drive_total[t] ≥ t_drive
```

**Time windows with exact boundaries:**

```
# Window [E, L] with exact times
# Work can only happen when: arrived AND within window

# Arrival must be ≤ L (latest departure minus work time)
arrival_time ≤ L - t_work

# Work starts at max(arrival_time, E)
work_start ≥ arrival_time
work_start ≥ E
work_start ≤ arrival_time + M·(arrival_time ≥ E)  # if early, wait until E

# Map work_start to period for x[t,WORK] activation
# Period p is active for work if: p·δ ≥ work_start AND p·δ < work_start + t_work
```

**Practical simplification:** For HoS verification, quantization error of ±2.5 min is acceptable. Round:
- Driving/work times: up to next δ (conservative)
- Time windows: inward (earliest up, latest down)

This guarantees feasible solutions remain feasible after rounding.

##### Problem-Specific Cuts and Branching for Ralph

HoS problems have domain structure that generic MIP cuts miss. Ralph should support:
1. **Cut callbacks** - user-defined cuts called during B&B
2. **Branching callbacks** - user-defined variable selection for branching

---

###### Ralph Callback API Design

**Cut Callback:**
```c
// Cut representation
typedef struct {
    int *indices;       // Variable indices
    double *coeffs;     // Coefficients
    int num_vars;       // Number of variables in cut
    char sense;         // 'L' (<=), 'G' (>=), 'E' (=)
    double rhs;         // Right-hand side
} RalphCut;

// Cut callback signature
typedef struct {
    // Called at each B&B node after LP relaxation solved
    // x_relaxation: current LP solution (fractional)
    // Returns number of cuts added (0 = no cuts found)
    int (*generate_cuts)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        RalphCut *cuts,         // Output: array of cuts
        int max_cuts            // Max cuts to generate
    );
    void *user_data;
} RalphCutCallback;

void ralph_set_cut_callback(RalphModel *model, RalphCutCallback *cb);
```

**Branching Priority Callback:**
```c
// Branching decision
typedef struct {
    int var_index;      // Variable to branch on (-1 = use default)
    double branch_point;// Value to branch at (usually 0.5 for binary)
    int direction;      // 0 = down first, 1 = up first
} RalphBranchDecision;

// Branching callback signature
typedef struct {
    // Called when B&B needs to select a branching variable
    // fractional_vars: indices of variables with fractional values
    // fractional_vals: their current LP values
    // Returns branching decision (var_index = -1 to use default)
    RalphBranchDecision (*select_branch)(
        void *user_data,
        const double *x_relaxation,
        const int *fractional_vars,
        const double *fractional_vals,
        int num_fractional
    );
    void *user_data;
} RalphBranchCallback;

void ralph_set_branch_callback(RalphModel *model, RalphBranchCallback *cb);
```

**Static Branching Priorities (simpler alternative):**
```c
// Set priority for each variable (higher = branch first)
// Useful when priorities are known upfront
void ralph_set_branch_priorities(RalphModel *model, const int *priorities);

// Set preferred direction for each variable
// 0 = down first, 1 = up first, -1 = auto
void ralph_set_branch_directions(RalphModel *model, const int *directions);
```

---

###### Concrete Cut Formulations for HoS

**Cut 1: Driving Capacity (11h limit)**

Detects: Fractional solution has > 11h driving between resets.

```
Given interval [t0, t1] with no reset10[τ] = 1 for τ ∈ [t0, t1]:

Violation detected when:
  ∑_{τ=t0}^{t1} x[τ,DRIVE] · δ > 11 + ε

Cut to add:
  ∑_{τ=t0}^{t1} x[τ,DRIVE] ≤ floor(11/δ)

Strengthened with reset variables:
  ∑_{τ=t0}^{t1} x[τ,DRIVE] ≤ floor(11/δ) + ceil(11/δ) · ∑_{τ=t0}^{t1} reset10[τ]
```

**Implementation:**
```c
int generate_driving_capacity_cuts(
    const double *x, int num_periods, double delta,
    int drive_var_offset, int reset_var_offset,
    RalphCut *cuts, int max_cuts
) {
    int num_cuts = 0;
    int last_reset = -1;
    double driving_sum = 0.0;

    for (int t = 0; t < num_periods && num_cuts < max_cuts; t++) {
        double x_drive = x[drive_var_offset + t];
        double x_reset = x[reset_var_offset + t];

        // Track fractional driving (works on LP relaxation)
        driving_sum += x_drive * delta;

        // Reset detected (even fractionally)
        if (x_reset > 0.5) {
            last_reset = t;
            driving_sum = 0.0;
            continue;
        }

        // Violation: driving exceeds 11h without reset
        if (driving_sum > 11.0 + 1e-6) {
            int t0 = last_reset + 1;
            int t1 = t;
            int cut_size = t1 - t0 + 1;

            // Build cut: ∑ x[τ,DRIVE] ≤ floor(11/δ)
            cuts[num_cuts].num_vars = cut_size;
            cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
            cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));
            for (int i = 0; i < cut_size; i++) {
                cuts[num_cuts].indices[i] = drive_var_offset + t0 + i;
                cuts[num_cuts].coeffs[i] = 1.0;
            }
            cuts[num_cuts].sense = 'L';
            cuts[num_cuts].rhs = floor(11.0 / delta);
            num_cuts++;

            // Reset for next segment
            driving_sum = 0.0;
            last_reset = t;
        }
    }
    return num_cuts;
}
```

**Cut 2: Mandatory Break (30-min after 8h driving)**

Detects: Fractional solution drives > 8h without a 30-min non-driving break.

```
Given interval [t0, t1] with no break30[τ] = 1 for τ ∈ [t0, t1]:

Violation detected when:
  ∑_{τ=t0}^{t1} x[τ,DRIVE] · δ > 8 + ε

Cut to add (at period t1 where violation detected):
  ∑_{τ=t0}^{t1} x[τ,DRIVE] ≤ floor(8/δ) + ceil(8/δ) · ∑_{τ=t0}^{t1} break30[τ]
```

**Implementation:**
```c
int generate_break_cuts(
    const double *x, int num_periods, double delta,
    int drive_var_offset, int break_var_offset,
    RalphCut *cuts, int max_cuts
) {
    int num_cuts = 0;
    int last_break = -1;
    double driving_sum = 0.0;

    for (int t = 0; t < num_periods && num_cuts < max_cuts; t++) {
        double x_drive = x[drive_var_offset + t];
        double x_break = x[break_var_offset + t];

        driving_sum += x_drive * delta;

        if (x_break > 0.5) {
            last_break = t;
            driving_sum = 0.0;
            continue;
        }

        // Violation: 8h driving without break
        if (driving_sum > 8.0 + 1e-6) {
            int t0 = last_break + 1;
            int t1 = t;
            int interval_len = t1 - t0 + 1;

            // Cut includes both DRIVE and BREAK variables
            int cut_size = 2 * interval_len;
            cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
            cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));

            // ∑ x[DRIVE] - floor(8/δ) · ∑ break30 ≤ floor(8/δ)
            for (int i = 0; i < interval_len; i++) {
                cuts[num_cuts].indices[i] = drive_var_offset + t0 + i;
                cuts[num_cuts].coeffs[i] = 1.0;
                cuts[num_cuts].indices[interval_len + i] = break_var_offset + t0 + i;
                cuts[num_cuts].coeffs[interval_len + i] = -floor(8.0 / delta);
            }
            cuts[num_cuts].num_vars = cut_size;
            cuts[num_cuts].sense = 'L';
            cuts[num_cuts].rhs = floor(8.0 / delta);
            num_cuts++;

            driving_sum = 0.0;
            last_break = t;
        }
    }
    return num_cuts;
}
```

**Cut 3: 14h Window**

This is better handled as **variable fixing** (preprocessing) rather than cuts:

```c
// At model construction time, fix variables to 0 where 14h would be exceeded
void fix_14h_window_violations(
    RalphModel *model, int num_periods, double delta,
    double w0,  // initial window usage
    int in_duty_init,
    int drive_var_offset
) {
    // If driver starts in duty period, window is already ticking
    double window_time = in_duty_init ? w0 : 0.0;
    int in_duty = in_duty_init;

    for (int t = 0; t < num_periods; t++) {
        // Once in_duty, window advances with wall clock
        if (in_duty) {
            window_time += delta;
        }

        // Can't drive if window would exceed 14h
        if (window_time > 14.0) {
            // Fix x[t,DRIVE] = 0
            ralph_set_var_bounds(model, drive_var_offset + t, 0.0, 0.0);
        }

        // Note: This is a simplification. Full model needs to track
        // when in_duty transitions based on activity and resets.
    }
}
```

For dynamic 14h tracking during B&B (when reset timing is uncertain), use a cut:

```
Given in_duty starts at t_start:

Violation detected when:
  in_duty[t] = 1 AND (t - t_start) · δ > 14 AND x[t,DRIVE] > 0

Cut to add:
  x[t,DRIVE] ≤ 1 - in_duty[t]   (if t is beyond 14h from any possible duty start)
```

---

###### Symmetry Breaking via Branching Priority

Symmetry: Multiple equivalent rest placements yield the same span. E.g., taking a 10h break at period 50 vs 51 may be equivalent.

**Solution: Branching priorities that prefer earlier decisions**

```c
// HoS branching priority: prefer earlier periods, prefer OFF over DRIVE
RalphBranchDecision hos_select_branch(
    void *user_data,
    const double *x,
    const int *frac_vars,
    const double *frac_vals,
    int num_frac
) {
    HSBranchContext *ctx = (HSBranchContext *)user_data;
    RalphBranchDecision decision = { .var_index = -1 };  // default

    int best_var = -1;
    int best_period = INT_MAX;
    double best_score = -1.0;

    for (int i = 0; i < num_frac; i++) {
        int var = frac_vars[i];
        double val = frac_vals[i];

        // Decode variable: which period and which state?
        int period = ctx->var_to_period[var];
        int state = ctx->var_to_state[var];  // DRIVE=0, WORK=1, OFF=2

        // Priority score: earlier periods first, OFF states preferred
        // (branching on OFF first explores "take rest early" branch)
        double score = (ctx->num_periods - period) * 10.0;
        if (state == STATE_OFF) score += 5.0;

        // Also consider fractionality (closer to 0.5 = harder to decide)
        double frac_score = 1.0 - fabs(val - 0.5) * 2.0;  // 1.0 at 0.5, 0.0 at 0 or 1
        score += frac_score;

        if (score > best_score) {
            best_score = score;
            best_var = var;
            best_period = period;
        }
    }

    if (best_var >= 0) {
        decision.var_index = best_var;
        decision.branch_point = 0.5;
        // Branch UP first for OFF variables (try taking rest early)
        // Branch DOWN first for DRIVE variables (try not driving)
        decision.direction = (ctx->var_to_state[best_var] == STATE_OFF) ? 1 : 0;
    }

    return decision;
}
```

**Static Priority Alternative:**
```c
// Set priorities at model construction (simpler, no callback needed)
void set_hos_branch_priorities(RalphModel *model, HSModelInfo *info) {
    int *priorities = calloc(info->num_vars, sizeof(int));
    int *directions = calloc(info->num_vars, sizeof(int));

    for (int t = 0; t < info->num_periods; t++) {
        // Earlier periods get higher priority
        int base_priority = (info->num_periods - t) * 10;

        // OFF variables: high priority, branch up first
        priorities[info->off_var[t]] = base_priority + 5;
        directions[info->off_var[t]] = 1;  // up first

        // DRIVE variables: medium priority, branch down first
        priorities[info->drive_var[t]] = base_priority + 3;
        directions[info->drive_var[t]] = 0;  // down first

        // WORK variables: lower priority
        priorities[info->work_var[t]] = base_priority + 1;
        directions[info->work_var[t]] = 0;
    }

    ralph_set_branch_priorities(model, priorities);
    ralph_set_branch_directions(model, directions);

    free(priorities);
    free(directions);
}
```

---

###### Cut Strength Analysis

| Cut Type | Strength | When to Generate | Cost |
|----------|----------|------------------|------|
| **Driving capacity (11h)** | Strong | Always—core constraint often violated in LP relaxation | O(H) scan |
| **Mandatory break (8h)** | Strong | Always—similar to driving capacity | O(H) scan |
| **14h window** | Medium | Preprocessing preferred; cut if reset timing uncertain | O(H) |
| **70h cycle** | Weak | Rarely needed—usually satisfied naturally | O(H) |
| **Symmetry breaking** | N/A | Use branching priority instead | N/A |

**Cut generation strategy:**
1. Generate at most 10-20 cuts per callback (diminishing returns)
2. Prioritize driving capacity and break cuts
3. Skip 14h and 70h cuts unless LP relaxation shows clear violation
4. Always use branching priorities for symmetry (not cuts)

---

###### Complete HoS Cut Callback Implementation

```c
// In hose/src/hs_mip_cuts.c

typedef struct {
    int num_periods;
    double delta;
    int drive_var_offset;
    int work_var_offset;
    int off_var_offset;
    int reset10_var_offset;
    int break30_var_offset;
    int *var_to_period;
    int *var_to_state;
} HSCutContext;

int hs_generate_cuts(
    void *user_data,
    const double *x,
    int num_vars,
    RalphCut *cuts,
    int max_cuts
) {
    HSCutContext *ctx = (HSCutContext *)user_data;
    int num_cuts = 0;
    int remaining = max_cuts;

    // 1. Driving capacity cuts (11h limit)
    int n = generate_driving_capacity_cuts(
        x, ctx->num_periods, ctx->delta,
        ctx->drive_var_offset, ctx->reset10_var_offset,
        cuts + num_cuts, remaining
    );
    num_cuts += n;
    remaining -= n;

    if (remaining <= 0) return num_cuts;

    // 2. Mandatory break cuts (8h limit)
    n = generate_break_cuts(
        x, ctx->num_periods, ctx->delta,
        ctx->drive_var_offset, ctx->break30_var_offset,
        cuts + num_cuts, remaining
    );
    num_cuts += n;
    remaining -= n;

    // 3. Skip 14h and 70h cuts (preprocessing handles most cases)

    return num_cuts;
}

// Setup callback
void hs_setup_callbacks(RalphModel *model, HSCutContext *cut_ctx, HSBranchContext *branch_ctx) {
    // Cut callback
    RalphCutCallback cut_cb = {
        .generate_cuts = hs_generate_cuts,
        .user_data = cut_ctx
    };
    ralph_set_cut_callback(model, &cut_cb);

    // Branching callback (or use static priorities)
    RalphBranchCallback branch_cb = {
        .select_branch = hos_select_branch,
        .user_data = branch_ctx
    };
    ralph_set_branch_callback(model, &branch_cb);

    // Alternative: static priorities (simpler)
    // set_hos_branch_priorities(model, branch_ctx->model_info);
}
```

---

###### Ralph Implementation Roadmap

**Phase 1: Static Branching Priorities**
```c
// Easy to implement, high impact for symmetry
void ralph_set_branch_priorities(RalphModel *model, const int *priorities);
void ralph_set_branch_directions(RalphModel *model, const int *directions);
```
- Store priorities in RalphModel
- Modify variable selection in branch_and_bound.c to use priorities
- ~50-100 LoC

**Phase 2: Lazy Constraints**
```c
// Simple cut addition between solves
int ralph_add_lazy_constraint(RalphModel *model, RalphCut *cut);
```
- Add constraint to model
- Re-solve from current basis (warm start)
- User loops: solve → check → add cuts → solve
- ~100 LoC

**Phase 3: Cut Callback**
```c
// Called automatically during B&B
void ralph_set_cut_callback(RalphModel *model, RalphCutCallback *cb);
```
- Invoke callback after each node LP solve
- Add returned cuts to node's constraint set
- More complex: need to manage cut pool, avoid duplicates
- ~300-500 LoC

**Phase 4: Branching Callback**
```c
// Full control over branching
void ralph_set_branch_callback(RalphModel *model, RalphBranchCallback *cb);
```
- Replace default variable selection with callback
- Pass fractional variable info to callback
- ~200 LoC

##### Adaptive Discretization (Advanced)

For very long horizons (multi-week), consider **variable δ**:
- Fine granularity (5 min) near task boundaries and time windows
- Coarse granularity (30 min or 1 hour) during long rest periods

This reduces period count significantly while maintaining precision where it matters.

**Implementation:** Pre-process the problem to identify "interesting" time points, then build a non-uniform time grid. Constraints adapt to variable period lengths.

---

#### Extension to Chain of Tasks

For a fixed sequence of N tasks (FTL with known legs):

**Option 1: Extended horizon**
- Single model with H = sum of worst-case spans
- Track task-specific completion variables: `arrived[t,k]`, `done[t,k]`
- Constraint: `done[t,k] = 1` before `x[t,DRIVE] = 1` for task k+1 can start

**Option 2: Iterative solving**
- Solve task 1 → get end state → solve task 2 → ...
- Faster but loses global optimality (local decisions may be suboptimal globally)
- Good enough for heuristic verification if tasks are loosely coupled

**Option 3: Hybrid**
- Solve globally for "critical" portions (tight time windows)
- Use heuristic + local verification for slack portions

---

#### Implementation Plan

**Phase 1: Time-Indexed Single Task**
- [ ] Define `HSVerifyProblem` struct (driver state, task, time window)
- [ ] Build MIP model using Ralph API
- [ ] Implement clock constraints (11h, 14h, 8h-break, 70h)
- [ ] Implement reset detection (10h, 34h, 30-min)
- [ ] Implement time window constraints (continuous first, recurring later)
- [ ] Extract solution as `HSAction` sequence
- [ ] Test against known scenarios (fresh driver, exhausted clocks, etc.)

**Phase 2: Heuristic Comparison**
- [ ] Run heuristic and MIP on same inputs
- [ ] Compare span: heuristic_span vs optimal_span
- [ ] Flag cases where heuristic_span > optimal_span × (1 + tolerance)
- [ ] Analyze corner cases to improve heuristic

**Phase 3: Chain Extension**
- [ ] Implement iterative solving with state propagation
- [ ] Implement global model for short chains (N ≤ 5)
- [ ] Compare iterative vs global for optimality gap

**Phase 4: Recurring Time Windows**
- [ ] Extend time-indexed model to handle daily open/close
- [ ] Test with realistic shipper appointment patterns

---

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

