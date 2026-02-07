# Tempo - Business Rules Engine

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

