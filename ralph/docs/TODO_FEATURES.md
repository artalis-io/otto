# Ralph Solver - Feature TODOs

This document outlines planned features for the Ralph LP/MIP solver with detailed implementation plans.

## Table of Contents

1. [Linear Assignment Problem (LAP) Solver](#1-linear-assignment-problem-lap-solver)
2. [Network Flow Solver](#2-network-flow-solver)
3. [Automatic Problem Detection and Delegation](#3-automatic-problem-detection-and-delegation)
4. [Benders Decomposition](#4-benders-decomposition)
5. [Dantzig-Wolfe Decomposition](#5-dantzig-wolfe-decomposition)
6. [External Solver Backends (HiGHS, GLPK)](#6-external-solver-backends-highs-glpk)

---

## 1. Linear Assignment Problem (LAP) Solver

### Problem Definition

The Linear Assignment Problem finds a minimum-cost one-to-one matching between n workers and n jobs:

```
minimize:    Σᵢ Σⱼ cᵢⱼ xᵢⱼ
subject to:  Σⱼ xᵢⱼ = 1    ∀i ∈ Workers  (each worker assigned exactly one job)
             Σᵢ xᵢⱼ = 1    ∀j ∈ Jobs     (each job assigned exactly one worker)
             xᵢⱼ ∈ {0,1}
```

### Current State ✅ IMPLEMENTED (2026-01-29)

Ralph now has a dedicated JVC (Jonker-Volgenant-Castanon) LAP solver with:
- **Dense solver**: O(n³) JVC algorithm with SIMD optimizations
- **Sparse solver**: Native sparse JVC for low-density problems (< 30%)
- **Rectangular support**: m × n problems (m ≤ n)
- **Warm start**: 2-10x speedup for similar consecutive problems
- **ε-scaling auction**: Optional tie-breaking for degenerate problems

**Performance (2026-01-29):**
| Size | Dense | Sparse (10%) | Warm Start (identical) |
|------|-------|--------------|------------------------|
| n=500 | 1.5ms | 1.1ms | 0.3ms (5x) |
| n=1000 | 10ms | 6.7ms | 1.5ms (7x) |
| n=2000 | 42ms | - | 6ms (7x) |

**Test coverage:** 146 tests (100% passing)

### Algorithm: Jonker-Volgenant-Castanon (JVC)

The JVC algorithm is the fastest known method for dense LAP, with O(n³) worst-case but often O(n²) in practice.

#### Algorithm Overview

```
JVC Algorithm:
1. INITIALIZATION: Compute initial dual variables
   - u[i] = min_j c[i][j] for each row i
   - v[j] = min_i (c[i][j] - u[i]) for each column j

2. AUGMENTATION PHASE: For each unassigned row
   a. Find shortest augmenting path using modified Dijkstra
   b. Augment along path (flip assignments)
   c. Update dual variables

3. Return assignment and optimal cost
```

#### Data Structures

```c
// lap.h

typedef struct {
    int n;              // Problem size (n×n)
    double *cost;       // Cost matrix (row-major, n×n)
    int *row_sol;       // row_sol[i] = column assigned to row i
    int *col_sol;       // col_sol[j] = row assigned to column j
    double *u;          // Dual variables for rows
    double *v;          // Dual variables for columns
    double optimal;     // Optimal cost
} LAPSolution;

typedef struct {
    // Working arrays for Dijkstra
    double *d;          // Shortest path distances
    int *pred;          // Predecessors
    int *scanned;       // Scanned flags
    double *min_to;     // Minimum reduced cost to reach column
} LAPWorkspace;
```

#### Core Implementation

```c
// lap.c

/**
 * Solve n×n linear assignment problem using JVC algorithm
 *
 * @param n         Problem size
 * @param cost      Cost matrix (n×n, row-major)
 * @param row_sol   Output: row_sol[i] = column assigned to row i (-1 if unassigned)
 * @param col_sol   Output: col_sol[j] = row assigned to column j (-1 if unassigned)
 * @return          Optimal cost, or RALPH_INFINITY if no solution
 */
double lap_solve(int n, const double *cost, int *row_sol, int *col_sol);

/**
 * Solve rectangular assignment problem (m workers, n jobs, m ≤ n)
 */
double lap_solve_rect(int m, int n, const double *cost, int *assignment);

/**
 * Solve LAP with forbidden assignments (cost = RALPH_INFINITY)
 */
double lap_solve_sparse(int n, int nnz, const int *rows, const int *cols,
                        const double *costs, int *row_sol, int *col_sol);
```

#### JVC Algorithm Details

**Phase 1: Column Reduction**
```c
// Initial dual feasible solution
for (j = 0; j < n; j++) {
    v[j] = INFINITY;
    for (i = 0; i < n; i++) {
        if (cost[i*n + j] < v[j]) {
            v[j] = cost[i*n + j];
            col_sol[j] = i;
        }
    }
}
// Handle ties and create initial (partial) assignment
```

**Phase 2: Reduction Transfer**
```c
// For rows assigned to unique columns, compute u[i]
for (i = 0; i < n; i++) {
    if (row_sol[i] >= 0) {
        int j = row_sol[i];
        u[i] = cost[i*n + j] - v[j];
    }
}
```

**Phase 3: Augmentation**
```c
// For each unassigned row, find shortest augmenting path
for (i = 0; i < n; i++) {
    if (row_sol[i] < 0) {
        // Modified Dijkstra from row i
        dijkstra_shortest_augmenting_path(i, cost, u, v, ...);
        // Augment and update duals
        augment_path(i, pred, row_sol, col_sol);
        update_duals(scanned, d, u, v);
    }
}
```

**Shortest Augmenting Path (Modified Dijkstra)**
```c
void dijkstra_shortest_augmenting_path(int start_row, const double *cost,
                                       const double *u, const double *v,
                                       double *d, int *pred, int *scanned,
                                       int n, int *row_sol, int *col_sol)
{
    // Initialize distances
    for (j = 0; j < n; j++) {
        d[j] = cost[start_row*n + j] - u[start_row] - v[j];
        pred[j] = start_row;
        scanned[j] = 0;
    }

    int sink = -1;
    while (sink < 0) {
        // Find unscanned column with minimum d[j]
        int j_min = -1;
        double d_min = INFINITY;
        for (j = 0; j < n; j++) {
            if (!scanned[j] && d[j] < d_min) {
                d_min = d[j];
                j_min = j;
            }
        }

        if (j_min < 0) break;  // No augmenting path (shouldn't happen for valid LAP)

        scanned[j_min] = 1;

        if (col_sol[j_min] < 0) {
            sink = j_min;  // Found unassigned column
        } else {
            // Relax through the row assigned to j_min
            int i = col_sol[j_min];
            for (j = 0; j < n; j++) {
                if (!scanned[j]) {
                    double new_d = d_min + cost[i*n + j] - u[i] - v[j];
                    if (new_d < d[j]) {
                        d[j] = new_d;
                        pred[j] = i;
                    }
                }
            }
        }
    }
}
```

#### Complexity Analysis

| Operation | Complexity |
|-----------|------------|
| Column reduction | O(n²) |
| Reduction transfer | O(n²) |
| Single augmentation | O(n²) (Dijkstra with linear scan) |
| Total | O(n³) worst-case |

With heap-based Dijkstra: O(n² log n) per augmentation, O(n³ log n) total.
But linear scan is faster for dense problems up to n ≈ 5000.

#### Sparse LAP Variant

For sparse cost matrices (many forbidden assignments):

```c
// Use adjacency list representation
typedef struct {
    int *adj_cols;      // Adjacent columns for each row
    double *adj_costs;  // Corresponding costs
    int *row_ptr;       // Pointers into adj arrays (CSR-like)
} SparseLAP;

double lap_solve_sparse_csr(const SparseLAP *lap, int *row_sol, int *col_sol);
```

### TODOs

- [x] Create `include/lap.h` with LAP solver API
- [x] Implement `lap_solve()` with JVC algorithm in `src/lap.c`
- [x] Implement column reduction phase
- [x] Implement augmenting path search with Dijkstra
- [x] Implement augmentation and dual update
- [x] Add rectangular assignment support `lap_solve_rect()`
- [x] Add sparse assignment support `lap_solve_sparse()`
- [x] Add heap-based Dijkstra option for large sparse problems
- [x] Create unit tests for LAP solver
- [x] Benchmark against simplex on LAP instances
- [x] **Warm start / incremental updates** (2026-01-29)
- [x] **SIMD optimizations** (dense, sparse, warm start validation)
- [x] **ε-scaling auction** for tie-breaking
- [x] **Cost matrix callbacks** - O(n) memory (2026-01-29)
- [x] **k-Best assignments** - Murty's algorithm (2026-01-29)
- [x] **MIP integration** - LAP-based LP relaxation in B&B (2026-01-29)
- [x] Integrate with problem detection (Section 3)

### Files Created

| File | Status |
|------|--------|
| `include/lap.h` | ✅ Created - LAP solver API |
| `src/lap.c` | ✅ Created - JVC implementation |
| `tests/test_lap.c` | ✅ Created - 213 unit tests |
| `benchmarks/bench_lap.c` | ✅ Created - LAP benchmarks |
| `include/detect.h` | ✅ Extended - MIP LAP detection |
| `src/detect.c` | ✅ Extended - MIP LAP solving |

### References

- Jonker, R., & Volgenant, A. (1987). "A shortest augmenting path algorithm for dense and sparse linear assignment problems"
- Castanon, D. A. (1991). "Efficient algorithms for finding the k best paths through a trellis"
- Murty, K. G. (1968). "An algorithm for ranking all the assignments in order of increasing cost"

### Future Improvements

| Feature | Priority | Complexity | Logistics Use Case |
|---------|----------|------------|-------------------|
| **Batched LAP** | High | Medium | Multi-frame tracking, parallel route assignments |
| **Bottleneck LAP** | Medium | Low | Load balancing (minimize worst driver workload) |
| **Online/Streaming LAP** | Medium | Medium | Real-time dispatch as orders arrive/cancel |
| **Constrained LAP** | High | Medium | Driver-vehicle assignment with shift/capacity limits |
| **3D Assignment** | Low | High | Multi-depot, multi-period fleet scheduling |
| **Auction Algorithm** | Low | Medium | Alternative solver for specific problem structures |
| **Approximate LAP** | Medium | Low | Fast heuristics for very large fleets (1000+ vehicles) |
| **GPU Acceleration** | Low | High | Massive-scale problems (n > 5000) |

**Logistics Assessment:**

1. **Batched LAP** - Solve driver-to-order assignments across multiple time windows simultaneously. High value for batch dispatch systems.

2. **Bottleneck LAP** - Minimize the longest route/shift rather than total cost. Ensures fairness and regulatory compliance (no driver overworked).

3. **Online LAP** - Critical for real-time dispatch. When a new order arrives or a driver becomes unavailable, update assignment incrementally rather than re-solving from scratch.

4. **Constrained LAP** - Real assignments have side constraints: driver certifications, vehicle capacities, time windows, mutual exclusions. This generalizes pure LAP to handle practical restrictions.

5. **3D Assignment** - Assign (driver, vehicle, shift) triples. Useful for multi-day planning where the same driver may use different vehicles on different days.

---

## 2. Network Flow Solver

### Problem Definition

**Minimum Cost Network Flow:**
```
minimize:    Σ(i,j)∈A cᵢⱼ xᵢⱼ
subject to:  Σⱼ xᵢⱼ - Σₖ xₖᵢ = bᵢ    ∀i ∈ N  (flow conservation)
             lᵢⱼ ≤ xᵢⱼ ≤ uᵢⱼ           ∀(i,j) ∈ A  (capacity)
```

Where:
- N = nodes, A = arcs
- bᵢ = supply (bᵢ > 0), demand (bᵢ < 0), or transshipment (bᵢ = 0)
- cᵢⱼ = cost per unit flow on arc (i,j)
- lᵢⱼ, uᵢⱼ = lower/upper capacity bounds

### Special Cases

| Problem | Characteristics |
|---------|-----------------|
| Shortest Path | Single source, single sink, unit flow |
| Maximum Flow | Zero costs, maximize flow s→t |
| Assignment | Bipartite, unit capacities, balanced supply/demand |
| Transportation | Bipartite, no transshipment nodes |
| Min-Cost Max-Flow | Cost minimization with max-flow constraint |

### Proposed Algorithm: Network Simplex

Network simplex exploits the spanning tree structure of basic feasible solutions.

#### Key Insight

For a network with n nodes and m arcs, any basic feasible solution corresponds to a spanning tree with n-1 basic arcs. This allows:
- O(n) basis representation (tree structure)
- O(n) pivoting (tree update)
- O(n) pricing (tree potentials)

vs. general simplex which requires O(m) for these operations.

#### Data Structures

```c
// netflow.h

typedef struct {
    int from;           // Source node
    int to;             // Target node
    double cost;        // Cost per unit flow
    double lower;       // Lower bound (usually 0)
    double upper;       // Upper bound (capacity)
} NFArc;

typedef struct {
    int num_nodes;
    int num_arcs;
    double *supply;     // Supply/demand at each node
    NFArc *arcs;        // Arc data
} NFProblem;

typedef struct {
    // Tree structure (basic solution)
    int *parent;        // parent[i] = parent node in spanning tree
    int *arc_to_parent; // arc_to_parent[i] = arc index connecting i to parent
    int *depth;         // depth[i] = depth in tree
    int *thread;        // thread[i] = next node in preorder traversal

    // Solution values
    double *flow;       // Flow on each arc
    double *potential;  // Node potentials (dual variables)

    // Arc status
    enum { LOWER, BASIC, UPPER } *arc_status;
} NFSolution;

typedef struct {
    NFProblem *problem;
    NFSolution *solution;

    // Working arrays
    int *pred;          // Predecessors for cycle detection
    double *delta;      // Flow change along cycle
} NFSolver;
```

#### Algorithm Overview

```
Network Simplex Algorithm:
1. INITIALIZATION: Find initial basic feasible solution
   - Add artificial arcs from/to a root node
   - Or use Big-M costs on artificial arcs

2. PRICING: Find entering arc
   - Compute reduced costs: rc[i,j] = c[i,j] - π[i] + π[j]
   - Select arc with most negative rc (for min)

3. RATIO TEST: Find leaving arc
   - Identify cycle formed by adding entering arc to tree
   - Compute max flow change δ along cycle
   - Leaving arc = arc that hits its bound first

4. PIVOT: Update tree and solution
   - Update flows along cycle
   - Update tree structure (parent, thread, depth)
   - Update potentials

5. Repeat until optimal (all rc ≥ 0) or unbounded
```

#### Tree Operations

**Potential Update (after pivot):**
```c
void update_potentials(NFSolver *solver, int entering_arc, int leaving_arc)
{
    // Only nodes in subtree rooted at one endpoint need update
    int subtree_root = /* identify which subtree moved */;
    double delta_pi = /* reduced cost of entering arc */;

    // Update all nodes in subtree using thread traversal
    int node = subtree_root;
    while (/* node is in subtree */) {
        solver->solution->potential[node] += delta_pi;
        node = solver->solution->thread[node];
    }
}
```

**Tree Update (pivot):**
```c
void update_tree(NFSolver *solver, int entering_arc, int leaving_arc)
{
    // 1. Remove leaving arc from tree
    // 2. Add entering arc to tree
    // 3. Update parent[], arc_to_parent[], depth[]
    // 4. Rethread for preorder traversal

    // The subtree that was connected via leaving arc
    // gets rerooted and attached via entering arc
}
```

#### Alternative: Push-Relabel Algorithm

For maximum flow problems (special case with zero costs):

```c
// maxflow.h

typedef struct {
    int num_nodes;
    int num_arcs;
    int source, sink;
    int *from, *to;
    double *capacity;
} MaxFlowProblem;

/**
 * Push-Relabel (Goldberg-Tarjan) maximum flow
 * O(V²E) basic, O(V³) with highest-label selection
 */
double maxflow_push_relabel(const MaxFlowProblem *problem, double *flow);

/**
 * Dinic's algorithm (blocking flow based)
 * O(V²E) general, O(E√V) for unit capacity
 */
double maxflow_dinic(const MaxFlowProblem *problem, double *flow);
```

### Network Simplex Implementation

#### Phase 1: Initialization

```c
int nf_initialize(NFSolver *solver)
{
    // Method 1: Artificial root with Big-M arcs
    // Add node n (root) with supply = -Σbᵢ
    // For each node i with bᵢ > 0: add arc (i, root) with cost M
    // For each node i with bᵢ < 0: add arc (root, i) with cost M
    // For each node i with bᵢ = 0: add arc (i, root) with cost M

    // Initial tree: all artificial arcs are basic
    // Initial flow: |bᵢ| on artificial arc for node i

    // Phase 1 complete when all artificial arcs have zero flow
}
```

#### Phase 2: Pricing

```c
int nf_select_entering(NFSolver *solver)
{
    double best_rc = -TOLERANCE;
    int best_arc = -1;

    for (int a = 0; a < solver->problem->num_arcs; a++) {
        if (solver->solution->arc_status[a] == BASIC) continue;

        int i = solver->problem->arcs[a].from;
        int j = solver->problem->arcs[a].to;
        double rc = solver->problem->arcs[a].cost
                  - solver->solution->potential[i]
                  + solver->solution->potential[j];

        // For arc at upper bound, negate reduced cost
        if (solver->solution->arc_status[a] == UPPER) {
            rc = -rc;
        }

        if (rc < best_rc) {
            best_rc = rc;
            best_arc = a;
        }
    }

    return best_arc;  // -1 if optimal
}
```

#### Phase 3: Ratio Test (Cycle Detection)

```c
void nf_find_cycle(NFSolver *solver, int entering_arc, int *cycle, int *cycle_len)
{
    int i = solver->problem->arcs[entering_arc].from;
    int j = solver->problem->arcs[entering_arc].to;

    // Find common ancestor of i and j in tree
    // Mark path from i to root
    int node = i;
    while (node >= 0) {
        solver->pred[node] = 1;  // mark
        node = solver->solution->parent[node];
    }

    // Walk from j until hitting marked node
    int ancestor = j;
    while (!solver->pred[ancestor]) {
        ancestor = solver->solution->parent[ancestor];
    }

    // Build cycle: i → ancestor → j → (entering arc) → i
    // ...
}

int nf_ratio_test(NFSolver *solver, int entering_arc, double *delta)
{
    // Compute flow change along cycle
    // Return leaving arc (the one that hits bound first)
}
```

### Complexity Analysis

| Operation | General Simplex | Network Simplex |
|-----------|-----------------|-----------------|
| Basis representation | O(m) | O(n) (tree) |
| Pricing | O(m) | O(m) reduced costs |
| Ratio test | O(m) | O(n) cycle |
| Pivot | O(m²) LU update | O(n) tree update |
| **Total per iteration** | O(m²) | O(m + n) |

For sparse networks (m = O(n)), network simplex is O(n) per iteration vs O(n²).

### TODOs

- [ ] Create `include/netflow.h` with network flow API
- [ ] Create `src/netflow.c` with network simplex core
- [ ] Implement spanning tree data structure with thread
- [ ] Implement tree potential computation
- [ ] Implement cycle detection and ratio test
- [ ] Implement tree pivot (structure update)
- [ ] Add Big-M initialization for infeasibility detection
- [ ] Implement max-flow specialization
- [ ] Add push-relabel algorithm (`src/maxflow.c`)
- [ ] Create unit tests for network flow
- [ ] Benchmark against general simplex on network problems
- [ ] Integrate with problem detection (Section 3)

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/netflow.h` | **Create** - Network flow API |
| `src/netflow.c` | **Create** - Network simplex implementation |
| `src/maxflow.c` | **Create** - Push-relabel max flow |
| `tests/test_netflow.c` | **Create** - Network flow tests |
| `benchmarks/bench_netflow.c` | **Create** - Network benchmarks |

### References

- Ahuja, R. K., Magnanti, T. L., & Orlin, J. B. (1993). "Network Flows: Theory, Algorithms, and Applications"
- Goldberg, A. V., & Tarjan, R. E. (1988). "A new approach to the maximum-flow problem"

---

## 3. Automatic Problem Detection and Delegation

### Motivation

Many real-world LPs have special structure that specialized algorithms can exploit:
- Assignment problems: O(n³) JVC vs O(n²m) simplex
- Network flows: O(nm) network simplex vs O(m²) general simplex
- Transportation: Specialized stepping-stone method

Ralph should automatically detect these structures and delegate to the appropriate solver.

### Detection Strategy

```
Problem Detection Pipeline:
1. Check matrix structure (dimensions, sparsity pattern)
2. Check constraint types (all =, all ≤, mixed)
3. Check coefficient values (all 0/±1, general)
4. Check variable bounds (binary, unit, general)
5. Map to problem class → Select solver
```

### Problem Signatures

#### Linear Assignment Problem (LAP)

**Signature:**
- n×n square assignment matrix
- All constraints are equality (=)
- Exactly 2n constraints (n row, n column)
- Each variable appears in exactly 2 constraints
- All coefficients are +1
- All RHS values are 1
- Optional: all variables binary (but LP relaxation is integral)

**Detection:**
```c
typedef struct {
    int is_lap;
    int n;              // Problem size
    int *row_to_var;    // Maps row index to variable indices
    int *col_to_var;    // Maps column index to variable indices
    double *costs;      // Cost matrix (extracted from objective)
} LAPSignature;

int detect_lap(const LPModel *model, LAPSignature *sig);
```

**Detection Algorithm:**
```c
int detect_lap(const LPModel *model, LAPSignature *sig)
{
    int m = model->num_cons;
    int n = model->num_vars;

    // Check: num_vars = num_cons² / 4 (n² vars, 2n constraints)
    // and num_cons is even
    if (m % 2 != 0) return 0;
    int size = m / 2;
    if (n != size * size) return 0;

    // Check: all equality constraints
    for (int i = 0; i < m; i++) {
        if (model->sense[i] != 'E') return 0;
        if (fabs(model->b[i] - 1.0) > TOLERANCE) return 0;
    }

    // Check: each variable appears in exactly 2 constraints with coef +1
    int *var_count = calloc(n, sizeof(int));
    // ... count appearances and verify coefficients ...

    // Check: constraints partition into row/column groups
    // ... verify bipartite structure ...

    // Extract cost matrix from objective
    // ...

    sig->is_lap = 1;
    sig->n = size;
    return 1;
}
```

#### Network Flow Problem

**Signature:**
- Constraint matrix has at most one +1 and one -1 per column
- This is the node-arc incidence matrix property
- All constraints are equality (pure network) or inequality (network with gains)
- Variables represent arc flows

**Detection:**
```c
typedef struct {
    int is_network;
    int num_nodes;
    int num_arcs;
    int *arc_from;      // Source node for each arc
    int *arc_to;        // Target node for each arc
    double *arc_cost;   // Cost for each arc
    double *arc_lower;  // Lower capacity
    double *arc_upper;  // Upper capacity
    double *supply;     // Node supply/demand
} NetworkSignature;

int detect_network(const LPModel *model, NetworkSignature *sig);
```

**Detection Algorithm:**
```c
int detect_network(const LPModel *model, NetworkSignature *sig)
{
    int m = model->num_cons;  // Nodes (after removing dependent constraint)
    int n = model->num_vars;  // Arcs

    // For each variable (column), check incidence pattern
    for (int j = 0; j < n; j++) {
        int plus_count = 0, minus_count = 0;
        int plus_row = -1, minus_row = -1;

        // Scan column j
        for (/* entries in column j */) {
            double val = /* coefficient */;
            int row = /* row index */;

            if (fabs(val - 1.0) < TOLERANCE) {
                plus_count++;
                plus_row = row;
            } else if (fabs(val + 1.0) < TOLERANCE) {
                minus_count++;
                minus_row = row;
            } else {
                return 0;  // Not ±1 coefficient
            }
        }

        // Valid patterns: (+1, -1), (+1, 0), (0, -1), (0, 0 for slack)
        if (plus_count > 1 || minus_count > 1) return 0;

        sig->arc_from[j] = minus_row;  // Flow leaves this node
        sig->arc_to[j] = plus_row;     // Flow enters this node
    }

    // Extract node supply from RHS
    for (int i = 0; i < m; i++) {
        sig->supply[i] = model->b[i];
        // Negate if >= constraint (demand)
    }

    sig->is_network = 1;
    sig->num_nodes = m;
    sig->num_arcs = n;
    return 1;
}
```

#### Transportation Problem

**Signature:**
- Bipartite network flow (sources → destinations, no intermediate nodes)
- Special case of network flow with simpler structure
- m sources, n destinations, m×n variables

**Detection:**
```c
int detect_transportation(const LPModel *model, TransportSignature *sig);
```

### Solver Dispatcher

```c
// dispatch.h

typedef enum {
    SOLVER_SIMPLEX,         // General LP
    SOLVER_DUAL_SIMPLEX,    // General LP (dual form)
    SOLVER_LAP_JVC,         // Linear assignment
    SOLVER_NETWORK_SIMPLEX, // Network flow
    SOLVER_PUSH_RELABEL,    // Max flow
    SOLVER_INTERIOR_POINT   // Future: barrier method
} SolverType;

typedef struct {
    SolverType recommended;
    SolverType fallback;
    double confidence;      // 0-1, how certain is the detection

    // Structure-specific data
    union {
        LAPSignature lap;
        NetworkSignature network;
    } signature;
} ProblemAnalysis;

/**
 * Analyze problem and recommend solver
 */
ProblemAnalysis analyze_problem(const LPModel *model);

/**
 * Solve using automatic solver selection
 * Falls back to simplex if specialized solver fails
 */
int solve_auto(LPModel *model);
```

### Integration with ralph_optimize()

```c
int ralph_optimize(RalphModel *model)
{
    // Get internal model
    LPModel *lp = model->lp_model;

    // Analyze problem structure
    ProblemAnalysis analysis = analyze_problem(lp);

    // Attempt specialized solver
    int status;
    switch (analysis.recommended) {
        case SOLVER_LAP_JVC:
            status = solve_as_lap(lp, &analysis.signature.lap);
            if (status == RALPH_STATUS_OPTIMAL) return 0;
            // Fall through to general solver
            break;

        case SOLVER_NETWORK_SIMPLEX:
            status = solve_as_network(lp, &analysis.signature.network);
            if (status == RALPH_STATUS_OPTIMAL) return 0;
            break;

        default:
            break;
    }

    // Default: general simplex
    return simplex_solve(lp);
}
```

### Configuration

Allow users to control detection behavior:

```c
// Parameters
ralph_set_int_param(model, "auto_detect", 1);     // Enable/disable
ralph_set_int_param(model, "detect_lap", 1);      // Enable LAP detection
ralph_set_int_param(model, "detect_network", 1);  // Enable network detection
ralph_set_dbl_param(model, "detect_threshold", 0.95);  // Confidence threshold
```

### TODOs

- [ ] Create `include/detect.h` with detection API
- [ ] Create `src/detect.c` with problem analysis
- [ ] Implement `detect_lap()` for assignment problems
- [ ] Implement `detect_network()` for network flows
- [ ] Implement `detect_transportation()` for transportation problems
- [ ] Create `include/dispatch.h` with solver dispatcher
- [ ] Create `src/dispatch.c` with automatic solver selection
- [ ] Integrate detection into `ralph_optimize()`
- [ ] Add solution mapping back to original variables
- [ ] Add parameters for enabling/disabling detection
- [ ] Create tests for problem detection
- [ ] Benchmark detection overhead

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/detect.h` | **Create** - Detection structures and API |
| `src/detect.c` | **Create** - Problem structure detection |
| `include/dispatch.h` | **Create** - Solver dispatcher API |
| `src/dispatch.c` | **Create** - Automatic solver selection |
| `src/ralph.c` | **Modify** - Integrate detection in optimize() |
| `tests/test_detect.c` | **Create** - Detection tests |

---

## 4. Benders Decomposition

### Problem Structure

Benders decomposition solves problems with complicating variables:

```
minimize:    c'x + f(y)
subject to:  Ax + By ≥ b
             x ∈ X, y ∈ Y
```

When x is fixed, the problem decomposes into a subproblem in y. Benders iteratively:
1. Solve a master problem (in x) with cuts
2. Solve subproblem(s) with fixed x
3. Add cuts to master based on subproblem duals

### Classic Two-Stage Stochastic Programming

```
Master (first stage):
minimize:    c'x + θ
subject to:  Ax ≥ b
             θ ≥ optimality cuts
             feasibility cuts
             x ∈ X

Subproblem (second stage, for fixed x̂):
minimize:    d'y
subject to:  Wy ≥ h - Tx̂
             y ≥ 0

Dual subproblem:
maximize:    π'(h - Tx̂)
subject to:  π'W ≤ d'
             π ≥ 0
```

### Cut Generation

**Optimality Cut** (when subproblem is feasible):
```
θ ≥ π*(h - Tx) = π*h - π*Tx
```
Where π* is the optimal dual solution.

**Feasibility Cut** (when subproblem is infeasible):
```
0 ≥ r*(h - Tx)
```
Where r* is an extreme ray (Farkas certificate) of the dual.

### Implementation

```c
// benders.h

typedef struct {
    LPModel *master;        // Master problem (in x)
    LPModel *subproblem;    // Subproblem template (in y, parameterized by x)

    // Linking structure
    int num_first_stage;    // Number of x variables
    int num_second_stage;   // Number of y variables
    SparseMatrix *T;        // Technology matrix (x affects RHS of subproblem)

    // Recourse matrix (W) is implicitly in subproblem->A
} BendersDecomp;

typedef struct {
    int max_iterations;
    double gap_tolerance;
    int verbose;
    int use_magnanti_wong;  // Pareto-optimal cuts
    int multi_cut;          // One θ per scenario vs single θ
} BendersParams;

typedef struct {
    int iterations;
    int optimality_cuts;
    int feasibility_cuts;
    double final_gap;
    double *x_solution;
    double *y_solution;
    double objective;
} BendersSolution;
```

#### Core Algorithm

```c
int benders_solve(BendersDecomp *decomp, const BendersParams *params,
                  BendersSolution *solution)
{
    double lower_bound = -INFINITY;
    double upper_bound = INFINITY;

    // Add θ variable to master (recourse cost estimate)
    int theta_idx = add_theta_variable(decomp->master);

    for (int iter = 0; iter < params->max_iterations; iter++) {
        // 1. Solve master problem
        int master_status = simplex_solve(decomp->master);
        if (master_status != RALPH_STATUS_OPTIMAL) {
            return master_status;  // Master infeasible or unbounded
        }

        double *x_hat = get_solution(decomp->master);
        double theta_hat = x_hat[theta_idx];
        lower_bound = get_objective(decomp->master);

        // 2. Update and solve subproblem
        update_subproblem_rhs(decomp, x_hat);
        int sub_status = simplex_solve(decomp->subproblem);

        if (sub_status == RALPH_STATUS_INFEASIBLE) {
            // 3a. Add feasibility cut
            double *farkas = get_farkas_ray(decomp->subproblem);
            add_feasibility_cut(decomp->master, farkas, decomp->T, x_hat);
            solution->feasibility_cuts++;

        } else if (sub_status == RALPH_STATUS_OPTIMAL) {
            double sub_obj = get_objective(decomp->subproblem);
            upper_bound = fmin(upper_bound, get_first_stage_cost(x_hat) + sub_obj);

            // 3b. Add optimality cut
            double *pi = get_dual_solution(decomp->subproblem);
            add_optimality_cut(decomp->master, theta_idx, pi, decomp->T);
            solution->optimality_cuts++;

            // Check convergence
            if (upper_bound - lower_bound < params->gap_tolerance) {
                solution->final_gap = upper_bound - lower_bound;
                copy_solution(solution, x_hat, decomp);
                return RALPH_STATUS_OPTIMAL;
            }
        }
    }

    return RALPH_STATUS_ITERATION_LIMIT;
}
```

#### Cut Management

```c
void add_optimality_cut(LPModel *master, int theta_idx,
                        const double *pi, const SparseMatrix *T)
{
    // Cut: θ ≥ π'h - π'Tx
    // Rearranged: θ + π'Tx ≥ π'h

    int nnz = /* count nonzeros in π'T */;
    int *indices = malloc((nnz + 1) * sizeof(int));
    double *values = malloc((nnz + 1) * sizeof(double));

    // θ coefficient
    indices[0] = theta_idx;
    values[0] = 1.0;

    // x coefficients from π'T
    int k = 1;
    for (int j = 0; j < T->ncols; j++) {
        double coef = 0.0;
        for (/* entries in column j of T */) {
            coef += pi[row] * T_value;
        }
        if (fabs(coef) > TOLERANCE) {
            indices[k] = j;
            values[k] = coef;  // Note: positive because moved to LHS
            k++;
        }
    }

    double rhs = /* π'h */;
    ralph_add_constraint(master, k, indices, values, RALPH_GREATER_EQUAL, rhs);

    free(indices);
    free(values);
}

void add_feasibility_cut(LPModel *master, const double *ray,
                         const SparseMatrix *T, const double *h)
{
    // Cut: r'(h - Tx) ≤ 0  →  r'Tx ≥ r'h
    // Similar structure to optimality cut but without θ
}
```

### Advanced Features

#### Magnanti-Wong Pareto-Optimal Cuts

Generate stronger cuts by solving auxiliary problem:
```c
void generate_pareto_cut(BendersDecomp *decomp, const double *x_hat,
                         const double *x_core, double *pi_pareto);
```

#### Multi-Cut Benders

For stochastic programming with multiple scenarios:
```c
typedef struct {
    int num_scenarios;
    double *probabilities;
    LPModel **subproblems;  // One per scenario
    int *theta_indices;     // One θ per scenario in master
} MulticutBenders;
```

#### Warm Starting

```c
// Reuse dual solution from previous iteration
void benders_warmstart_subproblem(BendersDecomp *decomp, const double *pi_prev);
```

### TODOs

- [ ] Create `include/benders.h` with Benders API
- [ ] Create `src/benders.c` with core Benders loop
- [ ] Implement optimality cut generation
- [ ] Implement feasibility cut generation (requires Farkas ray)
- [ ] Add subproblem RHS update from master solution
- [ ] Implement cut storage and management
- [ ] Add Magnanti-Wong Pareto-optimal cuts (optional)
- [ ] Add multi-cut variant for stochastic programming
- [ ] Add callback interface for custom cut generation
- [ ] Create tests for Benders decomposition
- [ ] Add examples (facility location, stochastic LP)

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/benders.h` | **Create** - Benders decomposition API |
| `src/benders.c` | **Create** - Benders algorithm implementation |
| `tests/test_benders.c` | **Create** - Benders tests |
| `examples/benders_facility.c` | **Create** - Facility location example |

### References

- Benders, J. F. (1962). "Partitioning procedures for solving mixed-variables programming problems"
- Magnanti, T. L., & Wong, R. T. (1981). "Accelerating Benders decomposition"
- Rahmaniani, R., et al. (2017). "The Benders decomposition algorithm: A literature review"

---

## 5. Dantzig-Wolfe Decomposition

### Problem Structure

Dantzig-Wolfe reformulates problems with block-angular structure:

```
Original:
minimize:    c₀'x₀ + Σₖ cₖ'xₖ
subject to:  A₀x₀ + Σₖ Aₖxₖ = b₀      (linking constraints)
             Bₖxₖ = bₖ, xₖ ∈ Xₖ       (block constraints)
```

The key insight: each xₖ can be expressed as a convex combination of extreme points of Xₖ:
```
xₖ = Σⱼ λₖⱼ x̂ₖⱼ,  where Σⱼ λₖⱼ = 1, λₖⱼ ≥ 0
```

### Master Problem (Restricted)

```
minimize:    c₀'x₀ + Σₖ Σⱼ (cₖ'x̂ₖⱼ) λₖⱼ
subject to:  A₀x₀ + Σₖ Σⱼ (Aₖx̂ₖⱼ) λₖⱼ = b₀    (linking)
             Σⱼ λₖⱼ = 1  ∀k                    (convexity)
             λₖⱼ ≥ 0
```

### Pricing Subproblem

For each block k, find a new extreme point with negative reduced cost:
```
minimize:    (cₖ - π'Aₖ)' xₖ - σₖ
subject to:  Bₖxₖ = bₖ
             xₖ ∈ Xₖ
```

Where π = dual prices for linking constraints, σₖ = dual price for convexity constraint k.

### Implementation

```c
// dw.h

typedef struct {
    int num_blocks;

    // Linking constraints
    LPModel *master;        // Restricted master problem
    SparseMatrix *A0;       // x₀ coefficients in linking
    double *c0;             // x₀ objective coefficients
    double *b0;             // Linking RHS

    // Block subproblems
    LPModel **subproblems;  // One per block
    SparseMatrix **Ak;      // Linking coefficients for each block

    // Column pool (extreme points)
    struct {
        double **points;    // Extreme point vectors
        double *costs;      // Reduced costs
        int count;
        int capacity;
    } *column_pool;         // One pool per block

} DWDecomp;

typedef struct {
    int max_iterations;
    double gap_tolerance;
    int max_columns_per_iter;  // Limit new columns
    int column_deletion;       // Remove non-basic columns
    int stabilization;         // Dual stabilization (boxstep, etc.)
} DWParams;

typedef struct {
    int iterations;
    int columns_generated;
    double final_gap;
    double objective;

    // Original variable solution (reconstructed from λ)
    double *x0_solution;
    double **xk_solutions;
} DWSolution;
```

#### Core Algorithm

```c
int dw_solve(DWDecomp *decomp, const DWParams *params, DWSolution *solution)
{
    double lower_bound = -INFINITY;
    double upper_bound = INFINITY;

    // Initialize with one column per block (artificial or heuristic)
    initialize_columns(decomp);

    for (int iter = 0; iter < params->max_iterations; iter++) {
        // 1. Solve restricted master problem
        int master_status = simplex_solve(decomp->master);
        if (master_status != RALPH_STATUS_OPTIMAL) {
            return master_status;
        }

        upper_bound = get_objective(decomp->master);

        // 2. Get dual prices
        double *pi = get_dual_linking(decomp->master);
        double *sigma = get_dual_convexity(decomp->master);

        // 3. Solve pricing subproblems
        int columns_added = 0;
        double min_reduced_cost = 0;

        for (int k = 0; k < decomp->num_blocks; k++) {
            // Update subproblem objective: c_k - π'A_k
            update_pricing_objective(decomp->subproblems[k], pi, decomp->Ak[k]);

            int sub_status = simplex_solve(decomp->subproblems[k]);
            if (sub_status != RALPH_STATUS_OPTIMAL) {
                // Handle unbounded or infeasible subproblem
                continue;
            }

            double sub_obj = get_objective(decomp->subproblems[k]);
            double reduced_cost = sub_obj - sigma[k];

            if (reduced_cost < -params->gap_tolerance) {
                // Add new column to master
                double *new_point = get_solution(decomp->subproblems[k]);
                add_column_to_master(decomp, k, new_point, reduced_cost);
                columns_added++;
                solution->columns_generated++;
            }

            min_reduced_cost = fmin(min_reduced_cost, reduced_cost);
        }

        // 4. Update lower bound
        lower_bound = upper_bound + min_reduced_cost;

        // 5. Check convergence
        if (upper_bound - lower_bound < params->gap_tolerance || columns_added == 0) {
            reconstruct_solution(decomp, solution);
            return RALPH_STATUS_OPTIMAL;
        }
    }

    return RALPH_STATUS_ITERATION_LIMIT;
}
```

#### Column Management

```c
void add_column_to_master(DWDecomp *decomp, int block, const double *point,
                          double reduced_cost)
{
    // New variable λ_{block,j}
    double obj = compute_original_cost(decomp, block, point);

    // Linking constraint coefficients: A_k x̂_{kj}
    double *link_coefs = malloc(decomp->num_linking * sizeof(double));
    sparse_matvec(decomp->Ak[block], point, link_coefs);

    // Add variable to master
    int var_idx = ralph_add_var(decomp->master, 0, INFINITY, obj, RALPH_CONTINUOUS);

    // Add to linking constraints
    for (int i = 0; i < decomp->num_linking; i++) {
        if (fabs(link_coefs[i]) > TOLERANCE) {
            add_to_constraint(decomp->master, i, var_idx, link_coefs[i]);
        }
    }

    // Add to convexity constraint for this block
    add_to_constraint(decomp->master, decomp->num_linking + block, var_idx, 1.0);

    // Store extreme point for solution reconstruction
    add_to_column_pool(decomp->column_pool[block], point, reduced_cost);

    free(link_coefs);
}

void reconstruct_solution(DWDecomp *decomp, DWSolution *solution)
{
    // Get λ values from master solution
    double *lambda = get_solution(decomp->master);

    // Reconstruct x_k = Σ_j λ_{kj} x̂_{kj}
    for (int k = 0; k < decomp->num_blocks; k++) {
        memset(solution->xk_solutions[k], 0, /* size */ * sizeof(double));

        for (int j = 0; j < decomp->column_pool[k].count; j++) {
            int lambda_idx = /* index of λ_{kj} in master */;
            double weight = lambda[lambda_idx];

            if (weight > TOLERANCE) {
                // x_k += λ_{kj} * x̂_{kj}
                axpy(weight, decomp->column_pool[k].points[j],
                     solution->xk_solutions[k], /* size */);
            }
        }
    }
}
```

### Advanced Features

#### Dual Stabilization (Boxstep)

Prevent oscillation of dual prices:
```c
typedef struct {
    double *pi_center;      // Stability center
    double delta;           // Box size
    int update_frequency;   // When to move center
} BoxstepStabilization;

void apply_boxstep(DWDecomp *decomp, BoxstepStabilization *stab);
```

#### Branch-and-Price

For integer programs:
```c
typedef struct {
    DWDecomp *dw;
    // Branching on λ variables or original x variables
    enum { BRANCH_LAMBDA, BRANCH_ORIGINAL } branching_strategy;
    // Column generation at each node
    int cg_max_iterations;
} BranchAndPrice;

int branch_and_price_solve(BranchAndPrice *bp, DWSolution *solution);
```

#### Column Deletion

Remove non-basic columns with high reduced cost:
```c
void delete_inactive_columns(DWDecomp *decomp, double threshold);
```

### Application: Vehicle Routing (VRPTW)

Classic DW application where subproblems are shortest path problems:

```c
typedef struct {
    int num_customers;
    int num_vehicles;
    double *demands;
    double *time_windows;
    double *distances;
    double capacity;
} VRPTWProblem;

// Master: set covering (each customer visited once)
// Subproblem: elementary shortest path with resource constraints (ESPPRC)
DWDecomp *vrptw_to_dw(const VRPTWProblem *vrp);
```

### TODOs

- [ ] Create `include/dw.h` with Dantzig-Wolfe API
- [ ] Create `src/dw.c` with core DW loop
- [ ] Implement restricted master problem setup
- [ ] Implement pricing subproblem objective update
- [ ] Implement column addition to master
- [ ] Implement solution reconstruction from λ values
- [ ] Add dual stabilization (boxstep method)
- [ ] Add column deletion for memory management
- [ ] Add warm-starting between iterations
- [ ] Create tests for DW decomposition
- [ ] Add examples (cutting stock, vehicle routing)
- [ ] (Optional) Branch-and-price for integer DW

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/dw.h` | **Create** - Dantzig-Wolfe API |
| `src/dw.c` | **Create** - DW algorithm implementation |
| `src/dw_pricing.c` | **Create** - Pricing subproblem utilities |
| `tests/test_dw.c` | **Create** - DW tests |
| `examples/dw_cutting_stock.c` | **Create** - Cutting stock example |

### References

- Dantzig, G. B., & Wolfe, P. (1960). "Decomposition principle for linear programs"
- Lübbecke, M. E., & Desrosiers, J. (2005). "Selected topics in column generation"
- Vanderbeck, F. (2000). "On Dantzig-Wolfe decomposition in integer programming"

---

## 6. External Solver Backends (HiGHS, GLPK)

### Motivation

While Ralph provides a zero-dependency LP/MIP solver suitable for embedded and WASM deployments, production environments may benefit from battle-tested external solvers:

| Solver | License | Strengths |
|--------|---------|-----------|
| **HiGHS** | MIT | State-of-the-art performance, active development, dual simplex, interior point, MIP |
| **GLPK** | GPL | Mature, widely used, good MIP, extensive documentation |

By providing a common API layer with pluggable backends, users can:
- Use Ralph for zero-dependency builds (WASM, embedded)
- Switch to HiGHS/GLPK for maximum performance in server environments
- Benchmark different solvers on the same problem
- Maintain a single codebase regardless of solver choice

### Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Application Code                         │
│                    (uses ralph.h API)                        │
├─────────────────────────────────────────────────────────────┤
│                   Ralph API Layer                            │
│              ralph_create(), ralph_optimize(), etc.          │
├─────────────────────────────────────────────────────────────┤
│                    Backend Dispatcher                        │
│                   (selects active backend)                   │
├─────────┬─────────────────┬─────────────────┬───────────────┤
│  Ralph  │     HiGHS       │      GLPK       │   Future...   │
│ Native  │    Backend      │     Backend     │   (SCIP,etc)  │
└─────────┴─────────────────┴─────────────────┴───────────────┘
```

### Backend Interface

```c
// backend.h

typedef enum {
    RALPH_BACKEND_NATIVE,   // Built-in Ralph solver (default)
    RALPH_BACKEND_HIGHS,    // HiGHS solver
    RALPH_BACKEND_GLPK,     // GLPK solver
} RalphBackend;

/**
 * Set the active solver backend
 * Must be called before ralph_create() or at program start
 */
int ralph_set_backend(RalphBackend backend);

/**
 * Get the currently active backend
 */
RalphBackend ralph_get_backend(void);

/**
 * Check if a backend is available (compiled in)
 */
int ralph_backend_available(RalphBackend backend);

/**
 * Get backend version string
 */
const char* ralph_backend_version(RalphBackend backend);
```

### Internal Backend Abstraction

```c
// backend_internal.h

typedef struct RalphBackendOps {
    /* Lifecycle */
    void* (*create)(void);
    void  (*free)(void *ctx);

    /* Model building */
    int (*add_var)(void *ctx, double lb, double ub, double obj, int type);
    int (*add_constraint)(void *ctx, int nnz, const int *indices,
                          const double *values, int sense, double rhs);
    int (*set_obj_sense)(void *ctx, int sense);

    /* Parameters */
    int (*set_int_param)(void *ctx, const char *name, int value);
    int (*set_dbl_param)(void *ctx, const char *name, double value);

    /* Solving */
    int (*optimize)(void *ctx);
    int (*get_status)(void *ctx);

    /* Solution retrieval */
    double (*get_objective)(void *ctx);
    int (*get_solution)(void *ctx, double *x);
    int (*get_dual)(void *ctx, double *pi);
    int (*get_reduced_cost)(void *ctx, double *rc);

    /* Advanced */
    int (*write_lp)(void *ctx, const char *filename);
    int (*write_mps)(void *ctx, const char *filename);
    int (*read_lp)(void *ctx, const char *filename);
    int (*read_mps)(void *ctx, const char *filename);

} RalphBackendOps;

/* Backend implementations */
extern const RalphBackendOps ralph_native_ops;
extern const RalphBackendOps ralph_highs_ops;
extern const RalphBackendOps ralph_glpk_ops;
```

### HiGHS Backend Implementation

```c
// backend_highs.c

#ifdef RALPH_WITH_HIGHS
#include <interfaces/highs_c_api.h>

typedef struct {
    void *highs;        // HiGHS model handle
    int num_vars;
    int num_cons;
} HiGHSContext;

static void* highs_create(void) {
    HiGHSContext *ctx = malloc(sizeof(HiGHSContext));
    ctx->highs = Highs_create();
    ctx->num_vars = 0;
    ctx->num_cons = 0;
    return ctx;
}

static void highs_free(void *context) {
    HiGHSContext *ctx = context;
    Highs_destroy(ctx->highs);
    free(ctx);
}

static int highs_add_var(void *context, double lb, double ub, double obj, int type) {
    HiGHSContext *ctx = context;
    HighsInt index;

    // HiGHS adds columns via Highs_addCol
    Highs_addCol(ctx->highs, obj, lb, ub, 0, NULL, NULL);
    index = ctx->num_vars++;

    // Set integer/binary type if needed
    if (type == RALPH_INTEGER || type == RALPH_BINARY) {
        Highs_changeColIntegrality(ctx->highs, index, kHighsVarTypeInteger);
    }

    return index;
}

static int highs_add_constraint(void *context, int nnz, const int *indices,
                                 const double *values, int sense, double rhs) {
    HiGHSContext *ctx = context;
    double lower, upper;

    // Convert Ralph sense to HiGHS bounds
    switch (sense) {
        case RALPH_LESS_EQUAL:    lower = -kHighsInf; upper = rhs; break;
        case RALPH_GREATER_EQUAL: lower = rhs; upper = kHighsInf; break;
        case RALPH_EQUAL:         lower = rhs; upper = rhs; break;
    }

    Highs_addRow(ctx->highs, lower, upper, nnz, (HighsInt*)indices, values);
    return ctx->num_cons++;
}

static int highs_optimize(void *context) {
    HiGHSContext *ctx = context;
    HighsInt status = Highs_run(ctx->highs);
    return (status == kHighsStatusOk) ? 0 : -1;
}

static int highs_get_status(void *context) {
    HiGHSContext *ctx = context;
    HighsInt model_status = Highs_getModelStatus(ctx->highs);

    switch (model_status) {
        case kHighsModelStatusOptimal:    return RALPH_STATUS_OPTIMAL;
        case kHighsModelStatusInfeasible: return RALPH_STATUS_INFEASIBLE;
        case kHighsModelStatusUnbounded:  return RALPH_STATUS_UNBOUNDED;
        default:                          return RALPH_STATUS_ERROR;
    }
}

static double highs_get_objective(void *context) {
    HiGHSContext *ctx = context;
    double obj;
    Highs_getObjectiveValue(ctx->highs, &obj);
    return obj;
}

static int highs_get_solution(void *context, double *x) {
    HiGHSContext *ctx = context;
    Highs_getSolution(ctx->highs, x, NULL, NULL, NULL);
    return 0;
}

const RalphBackendOps ralph_highs_ops = {
    .create = highs_create,
    .free = highs_free,
    .add_var = highs_add_var,
    .add_constraint = highs_add_constraint,
    .optimize = highs_optimize,
    .get_status = highs_get_status,
    .get_objective = highs_get_objective,
    .get_solution = highs_get_solution,
    // ... other ops
};

#endif /* RALPH_WITH_HIGHS */
```

### GLPK Backend Implementation

```c
// backend_glpk.c

#ifdef RALPH_WITH_GLPK
#include <glpk.h>

typedef struct {
    glp_prob *lp;
    int num_vars;
    int num_cons;
    int is_mip;
} GLPKContext;

static void* glpk_create(void) {
    GLPKContext *ctx = malloc(sizeof(GLPKContext));
    ctx->lp = glp_create_prob();
    ctx->num_vars = 0;
    ctx->num_cons = 0;
    ctx->is_mip = 0;
    return ctx;
}

static void glpk_free(void *context) {
    GLPKContext *ctx = context;
    glp_delete_prob(ctx->lp);
    free(ctx);
}

static int glpk_add_var(void *context, double lb, double ub, double obj, int type) {
    GLPKContext *ctx = context;
    int index = glp_add_cols(ctx->lp, 1);

    // Set bounds
    int bound_type;
    if (lb == -RALPH_INFINITY && ub == RALPH_INFINITY) {
        bound_type = GLP_FR;  // Free
    } else if (ub == RALPH_INFINITY) {
        bound_type = GLP_LO;  // Lower bound only
    } else if (lb == -RALPH_INFINITY) {
        bound_type = GLP_UP;  // Upper bound only
    } else if (lb == ub) {
        bound_type = GLP_FX;  // Fixed
    } else {
        bound_type = GLP_DB;  // Double bounded
    }
    glp_set_col_bnds(ctx->lp, index, bound_type, lb, ub);

    // Set objective coefficient
    glp_set_obj_coef(ctx->lp, index, obj);

    // Set variable type
    if (type == RALPH_INTEGER) {
        glp_set_col_kind(ctx->lp, index, GLP_IV);
        ctx->is_mip = 1;
    } else if (type == RALPH_BINARY) {
        glp_set_col_kind(ctx->lp, index, GLP_BV);
        ctx->is_mip = 1;
    }

    ctx->num_vars++;
    return index;
}

static int glpk_add_constraint(void *context, int nnz, const int *indices,
                                const double *values, int sense, double rhs) {
    GLPKContext *ctx = context;
    int row = glp_add_rows(ctx->lp, 1);

    // GLPK uses 1-based indexing
    int *ind = malloc((nnz + 1) * sizeof(int));
    double *val = malloc((nnz + 1) * sizeof(double));
    for (int i = 0; i < nnz; i++) {
        ind[i + 1] = indices[i] + 1;  // Convert to 1-based
        val[i + 1] = values[i];
    }

    glp_set_mat_row(ctx->lp, row, nnz, ind, val);

    // Set constraint bounds
    int bound_type;
    switch (sense) {
        case RALPH_LESS_EQUAL:    bound_type = GLP_UP; break;
        case RALPH_GREATER_EQUAL: bound_type = GLP_LO; break;
        case RALPH_EQUAL:         bound_type = GLP_FX; break;
    }
    glp_set_row_bnds(ctx->lp, row, bound_type, rhs, rhs);

    free(ind);
    free(val);
    ctx->num_cons++;
    return row;
}

static int glpk_optimize(void *context) {
    GLPKContext *ctx = context;

    if (ctx->is_mip) {
        glp_simplex(ctx->lp, NULL);  // Solve LP relaxation first
        return glp_intopt(ctx->lp, NULL);
    } else {
        return glp_simplex(ctx->lp, NULL);
    }
}

static int glpk_get_status(void *context) {
    GLPKContext *ctx = context;
    int status = ctx->is_mip ? glp_mip_status(ctx->lp) : glp_get_status(ctx->lp);

    switch (status) {
        case GLP_OPT:    return RALPH_STATUS_OPTIMAL;
        case GLP_INFEAS:
        case GLP_NOFEAS: return RALPH_STATUS_INFEASIBLE;
        case GLP_UNBND:  return RALPH_STATUS_UNBOUNDED;
        default:         return RALPH_STATUS_ERROR;
    }
}

const RalphBackendOps ralph_glpk_ops = {
    .create = glpk_create,
    .free = glpk_free,
    .add_var = glpk_add_var,
    .add_constraint = glpk_add_constraint,
    .optimize = glpk_optimize,
    .get_status = glpk_get_status,
    // ... other ops
};

#endif /* RALPH_WITH_GLPK */
```

### Build Configuration

```makefile
# Makefile additions

# Optional solver backends
RALPH_WITH_HIGHS ?= 0
RALPH_WITH_GLPK ?= 0

ifeq ($(RALPH_WITH_HIGHS),1)
    CFLAGS += -DRALPH_WITH_HIGHS
    LDFLAGS += -lhighs
    BACKEND_SRCS += src/backend_highs.c
endif

ifeq ($(RALPH_WITH_GLPK),1)
    CFLAGS += -DRALPH_WITH_GLPK
    LDFLAGS += -lglpk
    BACKEND_SRCS += src/backend_glpk.c
endif
```

**CMake alternative:**

```cmake
# CMakeLists.txt additions

option(RALPH_WITH_HIGHS "Enable HiGHS backend" OFF)
option(RALPH_WITH_GLPK "Enable GLPK backend" OFF)

if(RALPH_WITH_HIGHS)
    find_package(HiGHS REQUIRED)
    target_compile_definitions(ralph PRIVATE RALPH_WITH_HIGHS)
    target_link_libraries(ralph PRIVATE highs::highs)
    target_sources(ralph PRIVATE src/backend_highs.c)
endif()

if(RALPH_WITH_GLPK)
    find_package(GLPK REQUIRED)
    target_compile_definitions(ralph PRIVATE RALPH_WITH_GLPK)
    target_link_libraries(ralph PRIVATE ${GLPK_LIBRARIES})
    target_sources(ralph PRIVATE src/backend_glpk.c)
endif()
```

### Runtime Backend Selection

```c
// Example usage

#include "ralph.h"

int main() {
    // Check available backends
    printf("Native: %s\n", ralph_backend_available(RALPH_BACKEND_NATIVE) ? "yes" : "no");
    printf("HiGHS:  %s\n", ralph_backend_available(RALPH_BACKEND_HIGHS) ? "yes" : "no");
    printf("GLPK:   %s\n", ralph_backend_available(RALPH_BACKEND_GLPK) ? "yes" : "no");

    // Select backend (before creating models)
    if (ralph_backend_available(RALPH_BACKEND_HIGHS)) {
        ralph_set_backend(RALPH_BACKEND_HIGHS);
        printf("Using HiGHS %s\n", ralph_backend_version(RALPH_BACKEND_HIGHS));
    }

    // Use Ralph API as normal - backend is transparent
    RalphModel *model = ralph_create();
    ralph_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);
    // ... build model ...
    ralph_optimize(model);
    // ...
    ralph_free(model);

    return 0;
}
```

### Parameter Mapping

Map Ralph parameters to backend-specific equivalents:

```c
// param_map.c

typedef struct {
    const char *ralph_name;
    const char *highs_name;
    const char *glpk_name;
} ParamMapping;

static const ParamMapping int_params[] = {
    {"verbose",       "output_flag",        NULL},
    {"presolve",      "presolve",           NULL},
    {"threads",       "threads",            NULL},
    {"time_limit",    "time_limit",         NULL},
    {"iteration_limit", "simplex_iteration_limit", NULL},
    {NULL, NULL, NULL}
};

static const ParamMapping dbl_params[] = {
    {"gap_tolerance", "mip_rel_gap",        "mip_gap"},
    {"feasibility_tolerance", "primal_feasibility_tolerance", NULL},
    {"optimality_tolerance", "dual_feasibility_tolerance", NULL},
    {NULL, NULL, NULL}
};
```

### TODOs

**Phase 1: Backend Infrastructure**
- [ ] Create `include/backend.h` with backend selection API
- [ ] Create `src/backend.c` with dispatcher implementation
- [ ] Define `RalphBackendOps` interface
- [ ] Implement native backend wrapper (`src/backend_native.c`)
- [ ] Add build system support (Makefile, optional CMake)

**Phase 2: HiGHS Backend**
- [ ] Create `src/backend_highs.c`
- [ ] Implement model building (add_var, add_constraint)
- [ ] Implement solving and status mapping
- [ ] Implement solution retrieval
- [ ] Implement parameter mapping
- [ ] Test against native backend results

**Phase 3: GLPK Backend**
- [ ] Create `src/backend_glpk.c`
- [ ] Implement model building (handle 1-based indexing)
- [ ] Implement solving (LP and MIP paths)
- [ ] Implement solution retrieval
- [ ] Implement parameter mapping
- [ ] Test against native backend results

**Phase 4: Advanced Features**
- [ ] Add LP/MPS file I/O through backends
- [ ] Add dual solution retrieval
- [ ] Add reduced cost retrieval
- [ ] Add basis information retrieval
- [ ] Add callback support (where backends allow)

**Phase 5: Testing and Documentation**
- [ ] Create comparison test suite (same problem, all backends)
- [ ] Benchmark performance across backends
- [ ] Document backend-specific limitations
- [ ] Add examples showing backend selection

### Files to Create/Modify

| File | Action |
|------|--------|
| `include/backend.h` | **Create** - Backend selection API |
| `src/backend.c` | **Create** - Backend dispatcher |
| `src/backend_native.c` | **Create** - Native Ralph wrapper |
| `src/backend_highs.c` | **Create** - HiGHS integration |
| `src/backend_glpk.c` | **Create** - GLPK integration |
| `src/param_map.c` | **Create** - Parameter mapping |
| `include/ralph.h` | **Modify** - Add backend selection functions |
| `Makefile` | **Modify** - Add backend build options |
| `tests/test_backends.c` | **Create** - Backend comparison tests |

### Notes

**Zero-dependency promise**: The native Ralph backend remains the default and requires no external libraries. HiGHS and GLPK are strictly optional and only linked when explicitly enabled at build time.

**WASM compatibility**: Only the native backend is suitable for WASM builds. External backends require native compilation and linking.

**License considerations**:
- HiGHS: MIT license (permissive, commercial-friendly)
- GLPK: GPL license (copyleft, may have implications for proprietary use)

---

## Implementation Priority

Recommended order of implementation:

1. **External Solver Backends (HiGHS, GLPK)** - High priority
   - Enables production-grade performance immediately
   - Low risk (wraps proven solvers)
   - Backend abstraction benefits all future development
   - HiGHS especially valuable (MIT license, excellent MIP)

2. ~~**Linear Assignment (LAP)**~~ ✅ COMPLETED (2026-01-29)
   - JVC algorithm with SIMD, sparse, rectangular, warm start
   - 146 tests, full benchmarks
   - See `include/lap.h`, `src/lap.c`

3. **Problem Detection** - High priority
   - Foundation for automatic delegation
   - Benefits existing solvers immediately
   - Low implementation complexity

4. **Network Flow** - Medium priority
   - Significant speedup for network problems
   - More complex than LAP (tree data structures)
   - Network simplex well-documented

5. **Benders Decomposition** - Medium priority
   - Requires Farkas ray extraction (partially implemented)
   - Very useful for two-stage stochastic programming
   - Modular: can start with basic version

6. **Dantzig-Wolfe Decomposition** - Lower priority
   - Most complex implementation
   - Requires dynamic column management
   - Branch-and-price is even more complex

## Testing Strategy

For each feature:
1. **Unit tests**: Individual functions (detection, cuts, etc.)
2. **Correctness tests**: Compare results with general simplex
3. **Performance benchmarks**: Measure speedup over simplex
4. **Stress tests**: Large instances, edge cases

## Resources

### Linear Assignment
- Jonker & Volgenant (1987) original paper
- LAPJV reference implementation: https://github.com/gatagat/lap

### Network Flow
- Ahuja, Magnanti, Orlin (1993) textbook
- LEMON graph library: https://lemon.cs.elte.hu/

### Decomposition Methods
- Conforti, Cornuéjols, Zambelli (2014) "Integer Programming"
- Wolsey (1998) "Integer Programming"
- Nemhauser & Wolsey (1988) "Integer and Combinatorial Optimization"
