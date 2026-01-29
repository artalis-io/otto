# HoSE - Hours of Service Engine (Planned)

## Overview

**H**ours **o**f **S**ervice **E**ngine - A rule engine for computing driver Hours of Service (HoS) compliance under US FMCSA and EU EC/561 regulations.

**Status: PLANNED** - See `docs/TODO_FEATURES.md` for implementation details.

## Planned Location

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

## Core Functionality

Given a driver's current state and a required driving time, HoSE computes:

1. **Transit Duration**: Total time including mandatory breaks/rest
2. **End Driver State**: Updated clocks and status after transit
3. **ETA**: Estimated time of arrival accounting for breaks
4. **Action Sequence**: List of drive/break/rest actions with timestamps

## Regulations Covered

### US FMCSA

| Rule | Description |
|------|-------------|
| 11-Hour Driving | Max 11 hours driving after 10 consecutive hours off duty |
| 14-Hour Window | No driving after 14 hours on-duty following 10 hours off |
| 30-Minute Break | Required after 8 cumulative hours of driving |
| 60/70-Hour Limit | Max 60 hours in 7 days or 70 hours in 8 days |
| 34-Hour Restart | Resets weekly limit after 34+ consecutive hours off |

### EU EC/561

| Rule | Description |
|------|-------------|
| 4.5-Hour Driving | Max 4.5 hours before 45-minute break |
| 9-Hour Daily | Max 9 hours daily (extendable to 10 hours twice/week) |
| 56-Hour Weekly | Max 56 hours in single week |
| 90-Hour Fortnightly | Max 90 hours in any two consecutive weeks |
| 11-Hour Daily Rest | Min 11 hours rest in 24-hour period |
| 45-Hour Weekly Rest | Min 45 hours weekly rest |

## FMCSA Four-Clock Model

The FMCSA HoS rules are modeled as four independent clocks:

| Clock | Limit | Resets When |
|-------|-------|-------------|
| 8-hour break clock | 8 hours | 30+ minute off-duty break |
| 11-hour driving clock | 11 hours | 10+ hour off-duty period |
| 14-hour shift clock | 14 hours | 10+ hour off-duty period |
| 70-hour cycle clock | 70 hours | 34+ hour off-duty restart |

## Planned API

```c
// Initialize driver state
void hs_init_state(HSDriverState *state, HSRuleset ruleset);

// Compute transit with breaks
int hs_compute_transit(const HSDriverState *state, double net_driving,
                       time_t start_time, HSTransitResult *result);

// Apply action to state
void hs_apply_action(HSDriverState *state, const HSAction *action);

// Check driving feasibility
int hs_can_drive(const HSDriverState *state, double duration);

// Get required break
HSAction hs_get_required_break(const HSDriverState *state);

// Free resources
void hs_free_result(HSTransitResult *result);
```

## Data Structures

```c
typedef enum {
    HS_RULESET_FMCSA,       // US Federal
    HS_RULESET_EC561,       // EU
} HSRuleset;

typedef enum {
    HS_STATUS_OFF_DUTY,
    HS_STATUS_SLEEPER,
    HS_STATUS_DRIVING,
    HS_STATUS_ON_DUTY_NOT_DRIVING,
} HSStatus;

typedef struct {
    HSRuleset ruleset;
    HSStatus current_status;
    double driving_today;
    double on_duty_today;
    double driving_since_break;
    double driving_this_week;
    time_t shift_start;
    time_t last_break_end;
} HSDriverState;

typedef struct {
    HSActionType type;
    double duration;
    time_t start_time;
    time_t end_time;
} HSAction;

typedef struct {
    double net_driving_time;
    double total_transit_time;
    time_t eta;
    HSDriverState end_state;
    HSAction *actions;
    int num_actions;
    int feasible;
} HSTransitResult;
```

## Integration Points

| Component | Integration |
|-----------|-------------|
| **FuelWise** | Include HoS breaks in refueling route planning |
| **Velo** | Add HoS-aware travel time estimation |
| **Pulse** | Use for break insertion during plan execution |
| **Arbor** | Constraint checking during search |

## Implementation Priority

1. Core FMCSA 4-clock model
2. Basic transit simulation
3. Break insertion algorithm
4. EU EC/561 support
5. Pattern-based acceleration for long-haul

## References

- [FMCSA Hours of Service Regulations](https://www.fmcsa.dot.gov/regulations/hours-service/summary-hours-service-regulations)
- [Regulation (EC) No 561/2006](https://eur-lex.europa.eu/legal-content/EN/ALL/?uri=CELEX%3A32006R0561)
