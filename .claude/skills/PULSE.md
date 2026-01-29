# Pulse - Execution Tracker and PTA Engine (Planned)

## Overview

**P**lan **U**tilization and **L**ive **S**tate **E**stimator - An execution tracking engine that simulates forward through time to compute Predicted Time of Arrival (PTA), driver state, and scheduled actions.

**Status: PLANNED** - See `docs/TODO_FEATURES.md` for implementation details.

## Planned Location

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
│   ├── pl_optimize.c       # Optimal break placement
│   └── pl_report.c         # PTA reporting
├── tests/
│   └── test_pulse.c        # Unit tests
└── CLAUDE.md
```

## Core Functionality

Given a driver's state and a sequence of tasks, Pulse computes:

1. **PTA**: Predicted Time of Arrival at each stop
2. **Driver State**: HoS clocks at each point
3. **Scheduled Actions**: Drives, breaks, rest, service times
4. **Feasibility Alerts**: Will we violate HoS? Miss time windows?

## Key Distinction

| Component | Role |
|-----------|------|
| **Arbor** | Explores *possible* plans (search) |
| **Sigma** | Selects *which* plans to execute (optimization) |
| **Pulse** | Computes *what happens* when executing (simulation) |
| **HoSE** | Answers "can I drive X hours?" (rules) |
| **Tempo** | Answers "is time window satisfied?" (constraints) |

Pulse *orchestrates* HoSE and Tempo to simulate execution.

## Planned API

```c
// Simulate plan and compute PTAs
int pl_simulate(const PLPlan *plan, const HSDriverState *initial_state,
                PLSchedule *schedule);

// Simulate with automatic break insertion
int pl_simulate_with_breaks(const PLPlan *plan, const HSDriverState *initial_state,
                            int optimize_breaks, PLSchedule *schedule);

// Get PTA for specific task
time_t pl_get_pta(const PLSchedule *schedule, int task_index);

// Get driver state at task
HSDriverState pl_get_state_at_task(const PLSchedule *schedule, int task_index);

// Check time window violations
int pl_check_windows(const PLSchedule *schedule, int *violations, int *num_violations);

// Free resources
void pl_free_schedule(PLSchedule *schedule);
```

## Data Structures

```c
typedef enum {
    PL_TASK_DRIVE,
    PL_TASK_PICKUP,
    PL_TASK_DELIVERY,
    PL_TASK_FUEL,
    PL_TASK_WAIT,
    PL_TASK_BREAK,
    PL_TASK_REST,
} PLTaskType;

typedef struct {
    PLTaskType type;
    int location_id;
    int origin_id;              // For drive tasks
    int destination_id;

    double drive_duration;
    double drive_distance;
    double service_duration;

    time_t window_start;
    time_t window_end;
    int has_window;
} PLTask;

typedef struct {
    PLTask *tasks;
    int num_tasks;
    int driver_id;
    int vehicle_id;
    time_t plan_start;
} PLPlan;

typedef struct {
    PLTaskType type;
    int task_index;             // -1 for inserted breaks
    time_t start_time;
    time_t end_time;
    double duration;
    int location_id;
    char description[128];
    HSDriverState driver_state_after;
} PLAction;

typedef struct {
    PLAction *actions;
    int num_actions;

    time_t *task_pta;           // PTA for each task
    HSDriverState *task_state;  // Driver state at each task

    time_t plan_end_time;
    double total_drive_time;
    double total_wait_time;
    double total_break_time;
    double total_rest_time;

    int is_feasible;
    int num_warnings;
    char **warnings;
} PLSchedule;
```

## Simulation Algorithm

```
simulate(plan, initial_state):
    current_time = plan.start_time
    current_state = copy(initial_state)
    actions = []

    for each task in plan.tasks:
        task_pta[task] = current_time

        if task.type == DRIVE:
            remaining_drive = task.drive_duration

            while remaining_drive > 0:
                max_drive = hs_max_driving_time(current_state)

                if max_drive <= 0:
                    # Insert required break/rest
                    break_action = hs_get_required_break(current_state)
                    actions.append(break_action)
                    hs_apply_action(current_state, break_action)
                    current_time += break_action.duration
                else:
                    drive_now = min(remaining_drive, max_drive)
                    actions.append(Action(DRIVE, current_time, drive_now))
                    current_time += drive_now
                    remaining_drive -= drive_now

        elif task.type in [PICKUP, DELIVERY, FUEL]:
            # Check time window
            if task.has_window and current_time < task.window_start:
                wait_duration = task.window_start - current_time
                actions.append(Action(WAIT, current_time, wait_duration))
                current_time = task.window_start

            if task.has_window and current_time > task.window_end:
                add_warning("Missed window at task %d", task)

            # Service time
            actions.append(Action(task.type, current_time, task.service_duration))
            current_time += task.service_duration

    return Schedule(actions, task_pta, ...)
```

## Scheduling Objectives

```c
typedef enum {
    PL_OBJ_MIN_DURATION,            // Minimize total time
    PL_OBJ_MIN_DURATION_NO_DELAY,   // Minimize, reject any delay
    PL_OBJ_MIN_TOTAL_DELAY,         // Minimize sum of delays
    PL_OBJ_MIN_MAX_DELAY,           // Minimize worst-case delay
} PLScheduleObjective;
```

## Break Optimization

### Naive Strategy
Insert breaks when forced by HoS limits.

### DP-Based Optimal Strategy
Use dynamic programming to find optimal ITSK placement:
- Minimize total duration or delay
- Respect HoS constraints
- Satisfy time windows

### LP-Based Optimization
For simpler cases, formulate as small MIP:
- Decision variables: break placement
- Minimize wait time
- Subject to HoS and window constraints

## Integration Points

| Component | Integration |
|-----------|-------------|
| **HoSE** | Provides driving limits and break requirements |
| **Tempo** | Provides time window constraints |
| **Velo** | Provides drive durations between locations |
| **FuelWise** | Provides fuel stop locations and durations |
| **Ralph** | Optionally optimizes break placement |

## Use Cases

1. **Dispatch Planning**: "When will driver arrive at each stop?"
2. **What-If Analysis**: "What if we add this pickup?"
3. **ETA Updates**: "Driver at stop 2, update remaining ETAs"
4. **Compliance Check**: "Will this plan cause HoS violations?"
5. **Schedule Display**: "Show driver their break schedule"

## Workflow Integration

```
Arbor: Generate candidate plans
        ↓
Pulse: Simulate each plan -> compute PTAs, check feasibility
        ↓
Sigma: Select best plans (with accurate costs from Pulse)
        ↓
Pulse: Generate final schedules for selected plans
        ↓
Output: Driver schedules with PTAs and break times
```

## Implementation Priority

1. Core data structures
2. Basic forward simulation
3. HoSE integration for break insertion
4. Tempo integration for window checking
5. ITSK handling with DP
6. LP-based break optimization
