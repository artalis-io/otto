# Pulse - Execution Tracker and PTA Engine

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

