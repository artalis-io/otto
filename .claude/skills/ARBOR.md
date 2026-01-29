# Arbor - State-Space Search Engine (Planned)

## Overview

**A**lgorithmic **R**ecursive **B**ranching and **O**ptimization **R**untime - A generic state-space search framework for solving complex combinatorial problems through systematic exploration. Provides infrastructure for branching, pruning, bounding, and state management.

**Status: PLANNED** - See `docs/TODO_FEATURES.md` for implementation details.

## Planned Location

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
│   ├── ar_pool.c           # State pool management
│   └── ar_stats.c          # Search statistics
├── tests/
│   └── test_arbor.c        # Unit tests
└── CLAUDE.md
```

## Use Cases

| Problem | State | Branching | Pruning |
|---------|-------|-----------|---------|
| Vehicle Routing | Partial route + unvisited | Add next stop | Bound vs best |
| Scheduling | Partial schedule + unassigned | Assign task to slot | Constraint violation |
| Trip Planning | Location + time + HoS | Choose next leg | HoS/time window |

## Search Strategies

| Strategy | Description | Use Case |
|----------|-------------|----------|
| **DFS** | Depth-first, memory efficient | Find any solution fast |
| **Best-First** | Priority queue by bound | Optimal for B&B |
| **Beam Search** | Limited-width BFS | Quality/memory balance |
| **Diving** | DFS with restarts | Hybrid exploration |

## Planned API

```c
// Create search context
ARContext* ar_create(const ARCallbacks *callbacks, void *problem_data);

// Free context
void ar_free(ARContext *ctx);

// Run search
ARSearchResult ar_search(ARContext *ctx, const ARParams *params);

// Get best solution
ARState* ar_get_best_solution(const ARContext *ctx);

// Get statistics
ARStats ar_get_stats(const ARContext *ctx);

// Warm start with known solution
void ar_set_incumbent(ARContext *ctx, ARState *solution, double objective);

// Solution callback
void ar_on_solution(ARContext *ctx,
                    void (*callback)(ARState *solution, void *user_data),
                    void *user_data);
```

## Problem-Specific Callbacks

```c
typedef struct {
    // Create initial state
    ARState* (*init)(void *problem_data);

    // Free state
    void (*free_state)(ARState *state);

    // Clone state
    ARState* (*clone)(const ARState *state);

    // Check if state is complete
    int (*is_complete)(const ARState *state);

    // Get objective value
    double (*objective)(const ARState *state);

    // Compute lower bound
    double (*lower_bound)(const ARState *state);

    // Generate branches
    int (*branch)(const ARState *state, ARBranch **branches, int *num);

    // Apply branch
    ARState* (*apply_branch)(const ARState *state, const ARBranch *branch);

    // Check feasibility
    ARBoundResult (*check_feasibility)(const ARState *state);

    // Optional: custom pruning
    int (*should_prune)(const ARState *state, double best_known);

    // Optional: dominance check
    int (*dominates)(const ARState *state1, const ARState *state2);
} ARCallbacks;
```

## Data Structures

```c
typedef enum {
    AR_BOUND_FEASIBLE,      // May lead to solution
    AR_BOUND_PRUNED,        // Cannot improve best
    AR_BOUND_INFEASIBLE,    // Violates constraints
} ARBoundResult;

typedef enum {
    AR_RESULT_OPTIMAL,      // Proven optimal
    AR_RESULT_FEASIBLE,     // Found solution, not proven optimal
    AR_RESULT_INFEASIBLE,   // No solution exists
    AR_RESULT_LIMIT,        // Hit limit
} ARSearchResult;

typedef struct {
    uint64_t nodes_explored;
    uint64_t nodes_pruned;
    uint64_t solutions_found;
    double best_objective;
    double best_bound;
    double gap;
    double elapsed_seconds;
} ARStats;

typedef struct {
    int max_nodes;
    double time_limit;
    double gap_tolerance;
    int solution_limit;
    enum {
        AR_STRATEGY_DFS,
        AR_STRATEGY_BFS,
        AR_STRATEGY_BEST_FIRST,
        AR_STRATEGY_BEAM,
    } strategy;
    int beam_width;
    int verbose;
} ARParams;
```

## DFS Algorithm (Explicit Stack)

```
PROCEDURE TreeSearch(root, params):
    stack <- [root]
    solutions <- priority queue (max size N)

    WHILE stack not empty AND can_continue(params):
        node <- stack.pop()

        IF is_leaf(node):
            value <- evaluate_path(node)
            IF value is valid:
                solutions.insert(node, value)
        ELSE:
            children <- enumerate_children(node, params)
            children <- apply_selection(children, params)
            children <- apply_pruning(children, solutions, params)

            FOR each child in children (reverse order):
                stack.push(child)

    RETURN solutions
```

## Pruning Heuristics

### 1. Branching Factor Limits

```c
int branching_factors[] = {10, 8, 6, 4, 3, 2, 2, 2, ...};  // Per-depth
```

### 2. Remaining Objective Upper Bound

```c
double estimate_remaining(ARState *state, ARParams *params) {
    double remaining_time = params->horizon - state->elapsed;
    double best_rate = precomputed_max_rate;
    return remaining_time * best_rate;
}
```

### 3. Forced Coverage

Protect nodes on paths to prioritized actions.

## Solution Pool

```c
#define AR_MAX_SOLUTIONS 100

typedef struct {
    ARState *solutions[AR_MAX_SOLUTIONS];
    double objectives[AR_MAX_SOLUTIONS];
    int count;
} ARSolutionPool;
```

## Integration Points

| Component | Use Case |
|-----------|----------|
| **FuelWise** | Route optimization with refueling |
| **HoSE** | Finding feasible break schedules |
| **Tempo** | Scheduling with time windows |
| **Sigma** | Generating candidate plans |
| **Ralph** | Complement LP/MIP for combinatorics |

## Implementation Priority

1. Core state management and explicit-stack DFS
2. Child enumeration and selection
3. Pruning heuristics (branching factor, upper bound)
4. Solution pool management
5. Best-first and beam search
6. Warm start and dominance pruning
