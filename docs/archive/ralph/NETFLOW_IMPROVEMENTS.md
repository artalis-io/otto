# Network Simplex Improvement Ideas

This document outlines potential improvements for the network simplex solver, inspired by the LAP/JVC improvements in Ralph.

## Overview

The current `netflow.c` implements a basic network simplex algorithm. The improvements below would bring it to feature parity with the LAP solver and enable automatic detection of network structure in general LP models.

---

## 1. Network Structure Detection in LP Models

**Priority: High**

Similar to `detect_lap()` in `detect.c`, detect MCNF (Minimum Cost Network Flow) structure in general LP models.

### MCNF Signature in LP

A network flow LP has the form:
- **Variables**: One per arc (non-negative, optionally bounded)
- **Constraints**: One per node (flow conservation)
- **Constraint matrix structure**: Each variable (arc) appears in exactly 2 constraints:
  - Coefficient `+1` in the tail node's constraint (outflow)
  - Coefficient `-1` in the head node's constraint (inflow)

### Detection Algorithm

```c
typedef struct {
    int is_network;
    int num_nodes;
    int num_arcs;
    int *var_to_arc;      /* LP variable -> network arc */
    int *arc_tail;        /* arc -> tail node */
    int *arc_head;        /* arc -> head node */
    double *arc_cost;     /* from objective */
    double *arc_capacity; /* from variable bounds */
    double *node_supply;  /* from constraint RHS */
} NetworkSignature;

int detect_network(const LPModel *model, NetworkSignature *sig);
void detect_network_free(NetworkSignature *sig);
int solve_as_network(const NetworkSignature *sig, double *solution, double *obj);
```

### Detection Steps

1. Check each variable appears in exactly 2 constraints
2. Check coefficients are `+1` and `-1` (or `-1` and `+1`)
3. Build node-arc incidence from constraint structure
4. Extract costs from objective, bounds from variable bounds
5. Compute supplies from RHS (handling equality constraints)

### Integration Points

- `ralph_optimize()` can check for network structure before simplex
- `solve_as_network()` uses `ralph_netflow_solve()` internally
- Map network solution back to LP variables

---

## 2. Special Case Detection

**Priority: High**

Detect and optimize specific network flow variants:

### 2.1 Transportation Problem

**Signature:**
- Bipartite network (sources → sinks, no intermediate nodes)
- All arcs go from supply nodes to demand nodes
- No transshipment

**Benefit:** Can use specialized pricing, simpler tree structure

```c
typedef enum {
    RALPH_NETFLOW_GENERAL,
    RALPH_NETFLOW_TRANSPORTATION,
    RALPH_NETFLOW_ASSIGNMENT,
    RALPH_NETFLOW_SHORTEST_PATH,
    RALPH_NETFLOW_MAX_FLOW
} RalphNetflowType;

RalphNetflowType ralph_netflow_detect_type(const RalphNetflowProblem *prob);
```

### 2.2 Assignment Problem

**Signature:**
- Transportation with unit supplies/demands
- All supplies = 1, all demands = -1
- All capacities ≥ 1 (or infinite)

**Benefit:** Delegate to LAP solver (JVC) which is highly optimized

```c
/* In solve: */
if (detect_assignment(problem)) {
    return solve_via_lap(problem, result);  /* Uses ralph_lap_solve() */
}
```

### 2.3 Shortest Path Problem

**Signature:**
- Single source (supply = 1) and single sink (supply = -1)
- All other nodes have supply = 0
- Unit flow

**Benefit:** Can use Dijkstra/Bellman-Ford instead of full network simplex

### 2.4 Maximum Flow Problem

**Signature:**
- Single source and sink
- Objective: maximize flow on a specific "return" arc
- Or: minimize negative cost on source-adjacent arcs

**Benefit:** Can use push-relabel or augmenting path algorithms

---

## 3. Warm Start Support

**Priority: Medium**

Allow reusing previous solution when problem changes slightly.

### Use Cases

- Sensitivity analysis (varying costs/capacities)
- Re-optimization after adding/removing arcs
- Branch-and-bound for network MIPs

### API

```c
typedef struct {
    double *flow;        /* Previous flow values */
    double *potential;   /* Previous node potentials */
    int *state;          /* Arc states (basic/at_lower/at_upper) */
    int *parent;         /* Tree structure */
    int *pred_arc;
} RalphNetflowWarmStart;

RalphNetflowStatus ralph_netflow_warm_start(
    RalphNetflowWorkspace *ws,
    const RalphNetflowWarmStart *warm
);

/* Option to save solution for warm start */
typedef struct {
    /* ... existing fields ... */
    int save_for_warm_start;  /* Save basis in workspace */
} RalphNetflowOptions;
```

### Implementation

1. Validate warm start solution is still feasible
2. If feasible, skip Phase 1 (artificial arcs)
3. Repair infeasibilities locally if minor changes
4. Continue with Phase 2 from warm basis

---

## 4. k-Best Network Flows

**Priority: Medium**

Find the k best (lowest cost) feasible flows, similar to `ralph_lap_solve_k_best()`.

### Algorithm (Murty-style)

1. Find optimal flow F₁
2. For each subsequent flow Fₖ:
   - Partition search space by fixing some arc flows
   - Solve constrained subproblems
   - Select best among candidates

### API

```c
RalphNetflowStatus ralph_netflow_solve_k_best(
    const RalphNetflowProblem *problem,
    int k,
    double *flows,       /* k * num_arcs array */
    double *objectives,  /* k-element array */
    int *num_found       /* Actual number found (may be < k) */
);
```

### Use Cases

- Alternative routing in logistics
- Robustness analysis
- Diverse solution generation

---

## 5. Cost Scaling / Epsilon Scaling

**Priority: Medium**

Handle degenerate problems better, similar to LAP epsilon scaling.

### Problem

Degenerate pivots (zero flow change) can cause cycling or slow convergence.

### Solution: ε-Scaling

1. Scale costs by large factor (e.g., `n * max_cost`)
2. Solve with scaled costs
3. Progressively reduce scale factor
4. Final solve with original costs

### API

```c
typedef struct {
    /* ... existing fields ... */
    int use_scaling;           /* Enable cost scaling */
    double initial_epsilon;    /* Starting scale (0 = auto) */
    double epsilon_factor;     /* Reduction factor per phase */
} RalphNetflowOptions;
```

---

## 6. Bottleneck Network Flow

**Priority: Low**

Minimize the maximum arc cost used (minimax objective).

### Problem Statement

Find a feasible flow that minimizes `max{ cost[a] : flow[a] > 0 }`.

### Algorithm

Binary search on maximum cost:
1. Sort distinct costs
2. Binary search: for threshold T, check if feasible flow exists using only arcs with cost ≤ T
3. Return minimum feasible T

### API

```c
typedef enum {
    RALPH_NETFLOW_SUM,       /* Minimize sum of costs (standard) */
    RALPH_NETFLOW_BOTTLENECK /* Minimize maximum cost */
} RalphNetflowObjType;
```

---

## 7. Unified API

**Priority: Medium**

Single entry point supporting all features, similar to `ralph_lap_solve_ex()`.

### Proposed API

```c
/* Cost input types */
typedef enum {
    RALPH_NETFLOW_COST_ARRAYS,   /* Standard: tail/head/cost arrays */
    RALPH_NETFLOW_COST_CALLBACK  /* On-demand cost function */
} RalphNetflowCostType;

/* Algorithm selection */
typedef enum {
    RALPH_NETFLOW_ALG_AUTO,       /* Auto-detect best algorithm */
    RALPH_NETFLOW_ALG_SIMPLEX,    /* Network simplex */
    RALPH_NETFLOW_ALG_CAPACITY,   /* Capacity scaling */
    RALPH_NETFLOW_ALG_SSP,        /* Successive shortest path */
    RALPH_NETFLOW_ALG_K_BEST      /* k-best solutions */
} RalphNetflowAlgorithm;

/* Unified problem structure */
typedef struct {
    int num_nodes;
    int num_arcs;

    RalphNetflowCostType cost_type;

    /* For COST_ARRAYS */
    const int *tail;
    const int *head;
    const double *cost;
    const double *capacity;
    const double *lower;
    const double *supply;

    /* For COST_CALLBACK */
    double (*cost_fn)(int arc, void *user_data);
    void *user_data;

    RalphNetflowObjective objective;
    RalphNetflowObjType obj_type;  /* SUM or BOTTLENECK */
} RalphNetflowProblemEx;

/* Unified options */
typedef struct {
    RalphNetflowAlgorithm algorithm;
    int k;                    /* For k-best */
    int warm_start;           /* Use warm start from workspace */
    int save_for_warm_start;  /* Save solution for next warm start */
    int use_scaling;          /* Enable cost scaling */
    int64_t max_iterations;
    int verbosity;
} RalphNetflowOptionsEx;

/* Unified result */
typedef struct {
    RalphNetflowStatus status;
    int num_solutions;        /* 1 for standard, k for k-best */
    double *objectives;       /* Array of objectives */
    double *flows;            /* num_solutions * num_arcs */
    double *potentials;       /* Optional: node potentials */
    int64_t iterations;
} RalphNetflowResultEx;

RalphNetflowStatus ralph_netflow_solve_ex(
    const RalphNetflowProblemEx *problem,
    const RalphNetflowOptionsEx *options,
    RalphNetflowResultEx *result,
    RalphNetflowWorkspace *workspace
);
```

---

## 8. MIP Integration

**Priority: Medium**

Use network simplex for network-structured MIP relaxations, similar to `solve_lap_at_node()`.

### API

```c
typedef struct {
    NetworkSignature base;
    double *base_costs;
    double *base_supplies;
    void *netflow_workspace;
    int num_vars;
} MIPNetworkSignature;

int detect_network_mip(const LPModel *model, MIPNetworkSignature *sig);
void detect_network_mip_free(MIPNetworkSignature *sig);

/* Solve network relaxation at B&B node with variable bounds */
int solve_network_at_node(
    MIPNetworkSignature *sig,
    const double *lb,         /* Variable lower bounds */
    const double *ub,         /* Variable upper bounds */
    double *solution,
    double *obj_val
);
```

### Handling Variable Fixings

- `x[a] = 0`: Set capacity[a] = 0
- `x[a] = 1`: Set lower[a] = 1 (if binary), adjust supplies

---

## 9. Performance Optimizations

**Priority: Low**

### 9.1 Better Pricing

- **Steepest edge**: Price using reduced cost / pivot element
- **Partial pricing**: Only scan subset of arcs
- **Multiple pricing**: Enter multiple arcs per iteration

### 9.2 Better Tree Updates

- Maintain children lists for O(subtree) updates
- Use link-cut trees for O(log n) path operations

### 9.3 SIMD Optimization

- Vectorize reduced cost computation
- Vectorize flow updates along paths

---

## Implementation Priority

| Phase | Features | Effort |
|-------|----------|--------|
| 1 | Network detection in LP, Assignment delegation to LAP | Medium |
| 2 | Warm start, Transportation detection | Medium |
| 3 | k-best, Cost scaling | Medium |
| 4 | Unified API, MIP integration | High |
| 5 | Bottleneck, Algorithm variants | Low |

---

## Files to Modify/Create

| File | Changes |
|------|---------|
| `include/netflow.h` | Extended types, unified API |
| `src/netflow.c` | New algorithms, warm start |
| `include/detect.h` | Network detection types |
| `src/detect.c` | `detect_network()`, MIP integration |
| `tests/test_netflow.c` | Tests for new features |

---

## References

1. Ahuja, Magnanti, Orlin - "Network Flows: Theory, Algorithms, Applications" (1993)
2. Goldberg, Tarjan - "Finding Minimum-Cost Circulations by Canceling Negative Cycles" (1989)
3. Murty - "An Algorithm for Ranking All the Assignments" (1968)
