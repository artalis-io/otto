# Project-Level Feature TODOs

This document outlines planned features at the project level, including new components and infrastructure changes.

## Table of Contents

1. [Project Renaming](#1-project-renaming)
2. [HoSE - Hours of Service Engine](#2-hose---hours-of-service-engine)

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
| `api/` | Active | REST API server |
| `ui/` | Active | React frontend |

## Implementation Priority

1. **Project Renaming** - Low priority (cosmetic, do when convenient)
2. **HoSE Core** - High priority (essential for realistic trucking optimization)
3. **HoSE-FuelWise Integration** - High priority (after HoSE core)
