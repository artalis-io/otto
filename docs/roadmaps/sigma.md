# Sigma - Fleet Plan Selection Engine

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

