# Tempo - Business Rules Engine (Planned)

## Overview

**T**ime-window and **E**vent **M**anagement **P**olicy **O**rchestrator - A constraint evaluation engine for business rules beyond HoS regulations. Handles time windows, appointment scheduling, facility hours, and custom business policies.

**Status: PLANNED** - See `docs/TODO_FEATURES.md` for implementation details.

## Planned Location

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

## Constraint Types

| Category | Examples |
|----------|----------|
| **Time Windows** | Delivery windows (9am-5pm), pickup appointments |
| **Facility Hours** | Warehouse open hours, gate restrictions |
| **Service Times** | Loading/unloading durations, dwell time |
| **Appointment Slots** | Fixed appointment times, slot scheduling |
| **Blackout Periods** | Holidays, restricted hours |
| **Lead Times** | Minimum advance notice for appointments |
| **Capacity Limits** | Max trucks per hour at facility |
| **Max Transit** | Maximum time/distance between points |

## Appointment Window Types

| Type | Description | Example |
|------|-------------|---------|
| **Continuous** | Single open-close window | "9am-5pm on March 15" |
| **Recurring** | Weekly pattern | "Mon-Fri 8am-6pm" |
| **Recurring w/o Weekend** | Skips Sat/Sun | "Mon-Fri 9am-5pm" |

## Planned API

```c
// Check if facility is open at given time
int tp_is_facility_open(const TPFacility *facility, time_t when);

// Find next facility opening time
time_t tp_next_opening(const TPFacility *facility, time_t after);

// Evaluate constraints for activity at given time
TPEvalResult tp_evaluate(const TPConstraint *constraints, int num_constraints,
                         time_t activity_start, double activity_duration);

// Find feasible window for activity
int tp_find_feasible_window(const TPConstraint *constraints, int num_constraints,
                            time_t earliest, time_t latest,
                            double activity_duration,
                            TPTimeWindow *result);

// Intersect multiple time windows
int tp_intersect_windows(const TPTimeWindow *windows, int num_windows,
                         TPTimeWindow *result);
```

## Data Structures

```c
typedef enum {
    TP_WINDOW_CONTINUOUS,           // One-time window
    TP_WINDOW_RECURRING,            // Weekly pattern
    TP_WINDOW_RECURRING_NO_WEEKEND, // Weekly, skip Sat/Sun
} TPWindowType;

typedef struct {
    time_t start;
    time_t end;
} TPTimeWindow;

typedef struct {
    uint8_t days_of_week;       // Bitmask: bit 0 = Sunday
    int start_hour, start_min;
    int end_hour, end_min;
    time_t effective_from;
    time_t effective_until;
} TPRecurringSchedule;

typedef struct {
    int id;
    char name[64];
    TPRecurringSchedule *schedules;
    int num_schedules;
    TPTimeWindow *blackouts;
    int num_blackouts;
} TPFacility;

typedef struct {
    enum {
        TP_CONSTRAINT_TIME_WINDOW,
        TP_CONSTRAINT_APPOINTMENT,
        TP_CONSTRAINT_FACILITY_HOURS,
        TP_CONSTRAINT_MIN_DWELL,
        TP_CONSTRAINT_MAX_DWELL,
        TP_CONSTRAINT_LEAD_TIME,
    } type;
    int is_hard;
    double penalty;
} TPConstraint;

typedef struct {
    int feasible;
    int num_violations;
    double total_penalty;
    time_t earliest_feasible;
    time_t latest_feasible;
} TPEvalResult;
```

## Intermediate Tasks (ITSKs)

Intermediate tasks are actions inserted between main tasks:
- Mandatory driver breaks at specific locations
- Home time visits
- Fixed appointments mid-route
- Fuel stops when timing matters

```c
typedef struct {
    int id;
    time_t earliest;            // Window start
    time_t latest;              // Window end
    int has_window;
    double min_duration;        // Minimum service time
    double lat, lon;            // Optional location
    double max_transit_from;    // Max time to next task
    int is_required;            // Hard vs optional
} TPIntermediateTask;
```

## Integration Points

| Component | Integration |
|-----------|-------------|
| **FuelWise** | Fuel stop must be during station hours |
| **HoSE** | Break locations must be accessible |
| **Pulse** | Time window validation during simulation |
| **Arbor** | Constraint checking during search |

## Implementation Priority

1. Time window core (continuous, recurring)
2. Facility hours checking
3. Blackout/exclusion windows
4. Intermediate task support
5. Combined constraint evaluation

## Example Usage

```c
// Define a delivery time window
TPConstraint delivery = {
    .type = TP_CONSTRAINT_TIME_WINDOW,
    .data.window = {
        .start = parse_time("2024-03-15 09:00"),
        .end = parse_time("2024-03-15 17:00")
    },
    .is_hard = 1
};

// Evaluate if arrival at 10am is feasible
TPEvalResult result = tp_evaluate(&delivery, 1,
    parse_time("2024-03-15 10:00"), 30 * 60);

if (result.feasible) {
    printf("Delivery is feasible\n");
}
```
