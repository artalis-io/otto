# Sigma - Fleet Plan Selection Engine (Planned)

## Overview

**S**election and **I**ntegration for **G**lobal **M**ulti-assignment **A**llocation - A MIP-based engine that selects the optimal subset of candidate plans to cover all fleet requirements. Uses set covering/partitioning formulation to choose which plans to execute.

**Status: PLANNED** - See `docs/TODO_FEATURES.md` for implementation details.

## Planned Location

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
│   └── sg_pricing.c        # Column generation (optional)
├── tests/
│   └── test_sigma.c        # Unit tests
└── CLAUDE.md
```

## Problem Formulation

### Set Partitioning (each demand served exactly once)

```
minimize:    Σⱼ cⱼ xⱼ                    (total cost of selected plans)
subject to:  Σⱼ aᵢⱼ xⱼ = 1    ∀i        (each demand covered exactly once)
             xⱼ ∈ {0,1}                  (select or not)
```

### Set Covering (each demand served at least once)

```
minimize:    Σⱼ cⱼ xⱼ
subject to:  Σⱼ aᵢⱼ xⱼ ≥ 1    ∀i        (each demand covered at least once)
             xⱼ ∈ {0,1}
```

Where:
- j ∈ Plans (candidate plans from Arbor)
- i ∈ Demands (stops, customers, loads)
- cⱼ = cost of plan j
- aᵢⱼ = 1 if plan j serves demand i

## Additional Constraints

| Constraint | Description |
|------------|-------------|
| Vehicle availability | Each vehicle executes at most one plan |
| Driver assignment | Match drivers to compatible plans |
| Fleet capacity | Limit on total vehicles deployed |
| Balance constraints | Vehicles must return to depots |
| Time coordination | Shared resources can't overlap |

## Planned API

```c
// Create fleet optimization problem
SGProblem* sg_create(int num_demands, int num_vehicles);

// Add demand to be covered
int sg_add_demand(SGProblem *problem, const char *description, int is_required);

// Add candidate plan
int sg_add_plan(SGProblem *problem, double cost,
                const int *covered_demands, int num_covered,
                int vehicle_id, int driver_id);

// Add plan from Arbor state
int sg_add_plan_from_arbor(SGProblem *problem, const ARState *state,
                           double cost, const int *covered, int num_covered);

// Solve the problem
int sg_solve(SGProblem *problem, SGSolution *solution);

// Free resources
void sg_free_problem(SGProblem *problem);
void sg_free_solution(SGSolution *solution);
```

## Data Structures

```c
typedef struct {
    int id;
    char description[64];
    double penalty;             // Cost of not covering
    int is_required;            // Hard vs soft
} SGDemand;

typedef struct {
    int id;
    double cost;

    int *covered_demands;
    int num_covered;

    int vehicle_id;
    int driver_id;
    double duration;
    time_t start_time;
    time_t end_time;

    void *arbor_state;          // Link to Arbor
} SGPlan;

typedef struct {
    SGDemand *demands;
    int num_demands;

    SGPlan *plans;
    int num_plans;

    int num_vehicles;
    int num_drivers;
    double *vehicle_costs;

    int max_vehicles;
    int require_partition;      // Exactly once vs at least once
} SGProblem;

typedef struct {
    int *selected_plans;
    int num_selected;
    double total_cost;
    int *uncovered_demands;
    int num_uncovered;
    int *vehicle_plan;          // vehicle -> plan assignment
} SGSolution;
```

## Priority Handling

```c
typedef struct {
    int demand_id;
    int is_committed;           // Must be covered
    int is_prioritized;         // Should be covered if feasible
    double priority_weight;
} SGDemandPriority;
```

**MIP Formulation:**
- Committed: hard constraint `Σⱼ aᵢⱼ xⱼ = 1`
- Prioritized: soft constraint with penalty slack
- Optional: covered if beneficial

## Plan End Conditions

| Condition | Description |
|-----------|-------------|
| `LOADED_OR_EMPTY` | Plan can end loaded or empty |
| `EMPTY_TRUCK` | Must complete all deliveries |
| `EMPTY_WITH_PICKUP_OPTION` | Empty, can accept future pickup |
| `FORCED_TAKEHOME` | Must end at home location |

## Integration with Ralph

```c
int sg_solve(SGProblem *problem, SGSolution *solution) {
    RalphModel *model = ralph_create();
    ralph_set_obj_sense(model, RALPH_MINIMIZE);

    // Variables: x[j] = 1 if plan j selected
    for (int j = 0; j < problem->num_plans; j++) {
        ralph_add_var(model, 0, 1, problem->plans[j].cost, RALPH_BINARY);
    }

    // Coverage constraints
    for (int i = 0; i < problem->num_demands; i++) {
        // Add constraint: Σ x[j] where plan j covers demand i
        RalphSense sense = problem->require_partition ?
            RALPH_EQUAL : RALPH_GREATER_EQUAL;
        ralph_add_constraint(model, ...);
    }

    // Vehicle constraints: at most one plan per vehicle
    // ...

    ralph_optimize(model);
    // Extract solution...
    ralph_free(model);
}
```

## Column Generation (Advanced)

For very large plan sets:

1. Start with subset of plans
2. Solve LP relaxation
3. Get dual prices π for demand constraints
4. **Pricing subproblem**: Find plan with negative reduced cost
   - `reduced_cost = plan_cost - Σᵢ πᵢ × (plan covers i)`
   - This is where Arbor generates new plans guided by duals
5. Add promising plans, repeat
6. Solve final MIP

## Workflow

```
Demands + Fleet + Constraints
        ↓
Arbor: Generate candidate plans per vehicle
        ↓
Sigma: Build set covering MIP
        ↓
Ralph: Solve MIP
        ↓
Output: Vehicle → Plan assignments
```

## Integration Points

| Component | Integration |
|-----------|-------------|
| **Arbor** | Generates candidate plans |
| **Ralph** | Solves the MIP |
| **HoSE** | Plans must be HoS-feasible |
| **Tempo** | Plans must satisfy time windows |
| **FuelWise** | Plan costs include fuel optimization |

## Implementation Priority

1. Core data structures
2. Basic set covering MIP
3. Vehicle assignment constraints
4. Priority/committed demand handling
5. Column generation (advanced)
