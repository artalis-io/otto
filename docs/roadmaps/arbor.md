# Arbor - State-Space Search Engine

**A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime

### Overview

A generic state-space search framework for solving complex combinatorial problems through systematic exploration. Arbor provides the infrastructure for branching, pruning, bounding, and state management that can be specialized for different problem domains.

### Use Cases

| Problem | State | Branching | Pruning |
|---------|-------|-----------|---------|
| Vehicle Routing | Partial route + unvisited stops | Add next stop | Bound vs best known |
| Scheduling | Partial schedule + unassigned tasks | Assign task to slot | Constraint violation |
| Bin Packing | Partial packing + remaining items | Place item in bin | Capacity exceeded |
| Trip Planning | Current location + remaining legs | Choose next leg | HoS/time window violation |

### High-Level Architecture

```
arbor/
├── include/
│   ├── arbor.h             # Public API
│   ├── ar_types.h          # Data structures
│   ├── ar_state.h          # State management interface
│   ├── ar_branch.h         # Branching strategies
│   ├── ar_bound.h          # Bounding functions
│   └── ar_search.h         # Search algorithms
├── src/
│   ├── arbor.c             # Main API implementation
│   ├── ar_search_dfs.c     # Depth-first search
│   ├── ar_search_bfs.c     # Breadth-first search
│   ├── ar_search_best.c    # Best-first search
│   ├── ar_search_beam.c    # Beam search
│   ├── ar_search_bnb.c     # Branch and bound
│   ├── ar_pool.c           # State pool management
│   └── ar_stats.c          # Search statistics
├── tests/
│   └── test_arbor.c        # Unit tests
└── CLAUDE.md
```

### Core Concepts (Preliminary)

```c
// ar_types.h

/* Opaque state handle - actual state defined by problem domain */
typedef struct ARState ARState;

/* Branch: a choice point in the search tree */
typedef struct {
    int branch_id;
    void *branch_data;          /* Problem-specific branching data */
    double priority;            /* For best-first ordering */
    char description[64];       /* Human-readable description */
} ARBranch;

/* Bound result */
typedef enum {
    AR_BOUND_FEASIBLE,          /* State may lead to feasible solution */
    AR_BOUND_PRUNED,            /* State cannot improve on best known */
    AR_BOUND_INFEASIBLE,        /* State violates hard constraints */
} ARBoundResult;

/* Search result */
typedef enum {
    AR_RESULT_OPTIMAL,          /* Proven optimal found */
    AR_RESULT_FEASIBLE,         /* Feasible solution found (not proven optimal) */
    AR_RESULT_INFEASIBLE,       /* No feasible solution exists */
    AR_RESULT_LIMIT,            /* Hit time/node/memory limit */
} ARSearchResult;

/* Search statistics */
typedef struct {
    uint64_t nodes_explored;
    uint64_t nodes_pruned;
    uint64_t nodes_infeasible;
    uint64_t solutions_found;
    double best_objective;
    double best_bound;
    double gap;
    double elapsed_seconds;
} ARStats;

/* Search parameters */
typedef struct {
    int max_nodes;
    double time_limit;
    double gap_tolerance;
    int solution_limit;
    enum {
        AR_STRATEGY_DFS,        /* Depth-first (memory efficient) */
        AR_STRATEGY_BFS,        /* Breadth-first (level by level) */
        AR_STRATEGY_BEST_FIRST, /* Priority queue by bound */
        AR_STRATEGY_BEAM,       /* Limited width BFS */
        AR_STRATEGY_DIVING,     /* DFS with periodic restarts */
    } strategy;
    int beam_width;             /* For beam search */
    int verbose;
} ARParams;

/* Problem-specific callbacks */
typedef struct {
    /* Create initial state */
    ARState* (*init)(void *problem_data);

    /* Free state */
    void (*free_state)(ARState *state);

    /* Clone state */
    ARState* (*clone)(const ARState *state);

    /* Check if state is complete solution */
    int (*is_complete)(const ARState *state);

    /* Get objective value (lower is better for minimization) */
    double (*objective)(const ARState *state);

    /* Compute lower bound on best achievable from this state */
    double (*lower_bound)(const ARState *state);

    /* Generate branches (children) from state */
    int (*branch)(const ARState *state, ARBranch **branches, int *num_branches);

    /* Apply branch to state (returns new state) */
    ARState* (*apply_branch)(const ARState *state, const ARBranch *branch);

    /* Check feasibility (can prune early) */
    ARBoundResult (*check_feasibility)(const ARState *state);

    /* Free branch data */
    void (*free_branch)(ARBranch *branch);

    /* Optional: custom pruning beyond bound comparison */
    int (*should_prune)(const ARState *state, double best_known);

    /* Optional: dominance check (state1 dominates state2?) */
    int (*dominates)(const ARState *state1, const ARState *state2);

} ARCallbacks;
```

### Core API (Preliminary)

```c
// arbor.h

/* Search context */
typedef struct ARContext ARContext;

/**
 * Create search context
 */
ARContext* ar_create(const ARCallbacks *callbacks, void *problem_data);

/**
 * Free search context
 */
void ar_free(ARContext *ctx);

/**
 * Run search
 */
ARSearchResult ar_search(ARContext *ctx, const ARParams *params);

/**
 * Get best solution found
 */
ARState* ar_get_best_solution(const ARContext *ctx);

/**
 * Get search statistics
 */
ARStats ar_get_stats(const ARContext *ctx);

/**
 * Set incumbent (warm start with known solution)
 */
void ar_set_incumbent(ARContext *ctx, ARState *solution, double objective);

/**
 * Add callback for solution found events
 */
void ar_on_solution(ARContext *ctx, void (*callback)(ARState *solution, void *user_data),
                    void *user_data);
```

### Search Strategies

**Depth-First Search (DFS)**
```
- Memory efficient: O(depth) states in memory
- Finds solutions quickly but may not be optimal
- Good for finding any feasible solution
- Use explicit stack, NOT recursion (better for deep searches)
```

**Best-First Search**
```
- Explores most promising states first (by lower bound)
- Optimal for branch-and-bound
- Higher memory usage: O(nodes) states in memory
```

**Beam Search**
```
- Limited-width BFS: keep only top-k states per level
- Good balance of quality and memory
- Not guaranteed optimal
```

**Diving with Restarts**
```
- DFS to find solutions quickly
- Periodically restart from best unexplored state
- Hybrid of exploration and exploitation
```

### DFS Algorithm (Explicit Stack)

```
PROCEDURE TreeSearch(root, params):
    stack ← [root]
    solutions ← empty priority queue (by objective value, max size N)

    WHILE stack is not empty AND can_continue(params):
        node ← stack.pop()

        IF is_leaf(node):
            value ← evaluate_path(node)
            IF value is valid:
                solutions.insert(node, value)
        ELSE:
            children ← enumerate_children(node, params)
            children ← apply_selection(children, params)
            children ← apply_pruning(children, solutions, params)

            FOR each child in children (in reverse order for DFS):
                stack.push(child)

    RETURN solutions
```

### Child Enumeration

For each parent node:
1. Compute remaining time in planning horizon
2. Iterate candidate actions and intermediate task placements
3. For each candidate:
   - Compute **lower-bound** duration (fast filter)
   - If promising, compute **exact** duration (full validation)
   - Reject if exceeds time limit or violates hard constraints
4. Compute updated objective totals for each feasible child

```c
typedef struct {
    int *candidate_indices;     /* Which actions to try */
    int num_candidates;
    double *lower_bounds;       /* Fast feasibility filter */
    double remaining_horizon;   /* Time left in planning span */
} AREnumContext;
```

### Child Selection Heuristic

Selection ranks children and picks the top ones for expansion:

```c
typedef enum {
    AR_SELECT_MAX_VALUE,        /* By objective value */
    AR_SELECT_MAX_PROFIT,       /* By profit contribution */
    AR_SELECT_ROUND_ROBIN,      /* Alternate between strategies */
    AR_SELECT_MIN_COST,         /* By cost (for minimization) */
} ARSelectStrategy;

/* Round-robin: alternate between value and profit to diversify search */
```

If no feasible child is found, emit an **unassigned sentinel node** that marks this as a leaf.

### Pruning Heuristics (Composable)

Multiple pruning rules are applied in sequence:

**1. Max Branch Count (Branching Factor)**
- For depth `d`, keep only first `branching_factors[d]` children
- Returns PRUNE_ALL for remaining siblings once limit hit

```c
int branching_factors[] = {10, 8, 6, 4, 3, 2, 2, 2, ...};  /* Per-depth limits */
```

**2. Remaining Objective Upper Bound**
- Precompute best achievable objective rate per action
- For current node, estimate max possible remaining objective
- If `current + remaining + future < best_known`, prune

```c
/* Upper bound calculation */
double estimate_remaining(ARState *state, ARParams *params) {
    double remaining_time = params->horizon - state->elapsed;
    double best_rate = precomputed_max_rate;
    return remaining_time * best_rate;
}
```

**3. Forced Coverage (Override)**
- When certain actions are prioritized/committed, protect nodes on paths that include them
- Relax branch-count pruning to ensure at least one plan reaches each prioritized action
- Allow extra branches up to `max_extra_branches_per_prioritized` depth

### Stop Conditions

**Leaf Detection (node is complete):**
- Unassigned: no feasible children generated
- Max depth reached: `depth >= max_allowed_depth`
- Planning horizon reached: `elapsed >= max_total_duration`

**Global Search Stop:**
- Wall-clock time limit exceeded
- Node limit exceeded
- Solution count limit reached
- Gap tolerance achieved

```c
typedef struct {
    int max_nodes;
    double max_duration_ms;
    int max_solutions;
    double gap_tolerance;
} ARStopConditions;
```

### Solution Diversity

Maintain a **capped pool** of top-N solutions:
- New solutions replace worst if better
- Lower bound for pruning = worst solution in pool
- Prevents memory explosion on easy problems

```c
#define AR_MAX_SOLUTIONS 100

typedef struct {
    ARState *solutions[AR_MAX_SOLUTIONS];
    double objectives[AR_MAX_SOLUTIONS];
    int count;
} ARSolutionPool;
```

### Integration Points

| Component | Use Case |
|-----------|----------|
| **FuelWise** | Route optimization with refueling decisions |
| **HoSE** | Finding feasible break schedules |
| **Tempo** | Scheduling with time window constraints |
| **Ralph** | Complement LP/MIP for combinatorial subproblems |

### Example: Trip Planning Search

```c
/* State: current location, time, HoS state, remaining stops */
typedef struct {
    int current_stop;
    time_t current_time;
    HSDriverState driver_state;
    int *remaining_stops;
    int num_remaining;
    double total_cost;
} TripState;

/* Branch: choose next stop */
ARBranch* trip_branch(const ARState *state, int *num) {
    TripState *ts = (TripState*)state;
    *num = ts->num_remaining;
    ARBranch *branches = malloc(*num * sizeof(ARBranch));
    for (int i = 0; i < *num; i++) {
        branches[i].branch_id = ts->remaining_stops[i];
        branches[i].priority = /* distance or time to stop */;
    }
    return branches;
}

/* Lower bound: MST on remaining stops */
double trip_lower_bound(const ARState *state) {
    TripState *ts = (TripState*)state;
    return ts->total_cost + mst_cost(ts->remaining_stops, ts->num_remaining);
}
```

### TODOs

**Phase 1: Core Infrastructure**
- [ ] Define ARState and ARCallbacks interface
- [ ] Implement state pool with efficient allocation
- [ ] Implement explicit-stack DFS (not recursive)
- [ ] Add stop condition checking (time, nodes, solutions)

**Phase 2: Child Management**
- [ ] Implement child enumeration framework
- [ ] Implement lower-bound filtering for child pruning
- [ ] Implement exact feasibility checking
- [ ] Implement child selection strategies (value, profit, round-robin)

**Phase 3: Pruning Heuristics**
- [ ] Implement branching factor limits (per-depth)
- [ ] Implement remaining-objective upper bound pruning
- [ ] Implement prioritized/committed action protection
- [ ] Make pruning rules composable

**Phase 4: Solution Management**
- [ ] Implement solution pool with fixed capacity
- [ ] Implement solution insertion/eviction
- [ ] Use pool worst solution as lower bound for pruning
- [ ] Add solution callback mechanism

**Phase 5: Advanced Features**
- [ ] Implement best-first search with priority queue
- [ ] Implement beam search
- [ ] Implement warm start with incumbent
- [ ] Implement dominance-based pruning (optional)
- [ ] Add comprehensive search statistics

**Phase 6: Applications**
- [ ] Create trip planning example (integrate HoSE + Tempo)
- [ ] Create scheduling example
- [ ] Benchmark on realistic problem sizes

---

