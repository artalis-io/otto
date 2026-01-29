# LAP Solver Roadmap

Future improvements and enhancements for the Ralph LAP solver.

## Implemented Features

- [x] Dense JVC algorithm (O(n³))
- [x] Minimization and maximization (all variants)
- [x] Sparse LAP with native JVC algorithm
- [x] Sparse LAP with automatic dense fallback (density > 30%)
- [x] LP-based solver (for verification)
- [x] Workspace reuse API
- [x] SIMD optimizations (dense and sparse)
- [x] OpenMP parallelization (optional)
- [x] Rectangular LAP (m × n problems)
- [x] Runtime parallel enable/disable
- [x] Sparse vs dense benchmarks
- [x] ε-scaling auction for tie-breaking
- [x] Incremental updates / warm start
- [x] Cost matrix callbacks (O(n) memory)
- [x] Presolve detection (auto-detect LAP in LPs)
- [x] k-Best assignments (Murty's algorithm)

---

## Planned Improvements

### 1. ~~Sparse-Native JVC Implementation~~ ✅ COMPLETED
**Status: Implemented (2026-01-29)**

Native sparse JVC algorithm implemented with:
- CSR format used directly in all phases
- Only iterates over finite-cost edges
- Automatic density threshold (30%) for switching between native sparse and dense conversion
- SIMD optimizations in initialization and Dijkstra minimum-finding
- Three-state Dijkstra (not reached / in queue / finalized) for correct infeasibility detection

**API:** `ralph_lap_solve_sparse()` - same function handles both native sparse and dense fallback

**Performance Results:**
| Size | Density | Native Sparse | Dense Conversion | Speedup |
|------|---------|---------------|------------------|---------|
| 500 | 10% | 1.1ms | 1.7ms | 1.5x |
| 1000 | 10% | 6.7ms | 9.5ms | 1.4x |
| 5000 | 10% | 417ms | - | native only |

**Benchmarks:** `bench_sparse_vs_dense()` and `bench_sparse_scaling()` in `benchmarks/bench_lap.c`

---

### 2. ~~Incremental Updates / Warm Start~~ ✅ COMPLETED
**Status: Implemented (2026-01-29)**

Warm start implementation for solving similar LAPs repeatedly:
- Automatically saves dual variables (u, v) and solution in workspace
- Reuses previous solution as starting point when costs change
- Validates previous solution against new cost matrix
- Falls back to cold start when warm start invalid or counterproductive

**API:**
```c
/* Solve with automatic warm start (uses workspace state) */
RalphLapStatus ralph_lap_solve_warm(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost,
    RalphLapWorkspace *ws,
    int use_warm_start     /* 1 to try warm start, 0 for cold */
);

/* Manual warm start initialization */
RalphLapStatus ralph_lap_warm_init(
    RalphLapWorkspace *ws,
    int n,
    const int *row_sol,
    const double *u,
    const double *v
);
```

**Performance Results:**
| Scenario | Speedup | Notes |
|----------|---------|-------|
| Identical problems | 6-10x | Best case - solution still valid |
| 1% perturbation | ~2x | Minor cost changes |
| 10% perturbation | ~0.6x | Overhead exceeds benefit |

**Benchmarks:** `bench_warm_start()` in `benchmarks/bench_lap.c`

---

### 3. ~~Cost Matrix Callbacks~~ ✅ COMPLETED
**Status: Implemented (2026-01-29)**

Callback-based solver for very large problems without storing full n² matrix:
- Compute costs on-demand via callback function
- Uses O(n) memory instead of O(n²)
- Full JVC algorithm with callback cost access

**API:**
```c
typedef double (*RalphLapCostFn)(int i, int j, void *user_data);

RalphLapStatus ralph_lap_solve_callback(
    int n,
    RalphLapCostFn cost_fn,
    void *user_data,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost
);

/* With workspace for repeated solves */
RalphLapStatus ralph_lap_solve_callback_with_workspace(
    int n,
    RalphLapCostFn cost_fn,
    void *user_data,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost,
    RalphLapWorkspace *ws
);
```

**Performance Results:**
| Size | Dense (ms) | Callback (ms) | Overhead |
|------|------------|---------------|----------|
| 100 | 0.05 | 0.10 | 2x |
| 500 | 1.4 | 3.4 | 2.4x |
| 1000 | 8.2 | 18.6 | 2.3x |

**Benefits:**
- Memory: O(n) instead of O(n²)
- Enables tracking/assignment with 10k+ objects
- Integrates with external data sources (databases, spatial indices)

**Benchmarks:** `bench_callback()` in `benchmarks/bench_lap.c`

---

### 4. ~~ε-Scaling Auction Algorithm~~ ✅ COMPLETED
**Status: Implemented**

Optional ε-scaling for tie-breaking in degenerate problems:
- Progressively refines solution through decreasing epsilon values
- Configurable scaling factor

**API:**
```c
ralph_lap_set_epsilon_scaling(1);      /* Enable */
ralph_lap_set_epsilon_factor(4.0);     /* Set scaling factor (default: 4.0) */
```

---

### 5. ~~k-Best Assignments~~ ✅ COMPLETED
**Status: Implemented (2026-01-29)**

Murty's algorithm implementation for finding k best assignments:
- Systematic partitioning of solution space
- Priority queue for ordered extraction of best solutions
- Supports both minimization and maximization
- Returns solutions in cost order (ascending for min, descending for max)

**API:**
```c
RalphLapStatus ralph_lap_solve_k_best(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int k,
    int *solutions,       /* k×n array of assignments */
    double *costs,        /* k costs */
    int *num_found        /* Actual number found (may be < k) */
);

/* With workspace for repeated solves */
RalphLapStatus ralph_lap_solve_k_best_with_workspace(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int k,
    int *solutions,
    double *costs,
    int *num_found,
    RalphLapWorkspace *ws
);
```

**Algorithm:**
1. Solve base LAP to get optimal assignment
2. Create partition nodes by fixing prefixes and forbidding one edge
3. Use min-heap (max-heap for maximize) priority queue
4. Extract best from queue, add to results, partition further
5. Repeat until k solutions found or queue exhausted

**Benefits:**
- Multi-hypothesis tracking (radar, vision systems)
- Solution diversity for robust planning
- Finding alternative assignments for sensitivity analysis

**Complexity:**
- Time: O(k × n³) - each of k solutions requires O(n³) LAP solve
- Space: O(k × n) for solutions + O(n²) for exclusion lists

---

## Ralph Solver Integration

### 6. ~~Presolve Detection~~ ✅ COMPLETED
**Status: Implemented (2026-01-29)**

Automatic LAP detection in general LP problems:
- Detects assignment structure via bipartite graph coloring
- Extracts cost matrix from LP objective coefficients
- Automatically delegates to JVC solver when LAP structure detected
- Supports both minimize and maximize objectives

**API:**
```c
/* Detection API (include/detect.h) */
int detect_lap(const LPModel *model, LAPSignature *sig);
void detect_lap_free(LAPSignature *sig);
int solve_as_lap(const LAPSignature *sig, double *solution, double *obj_val);

/* Runtime enable/disable */
void ralph_set_detect_lap(int enabled);    /* Global toggle (default: enabled) */
ralph_set_int_param(model, "detect_special", 1);  /* Per-model (default: disabled) */
```

**Detection criteria:**
- 2n constraints (n row + n column)
- n² variables
- Each variable in exactly 2 constraints with coefficient +1
- All constraints equality with RHS = 1
- Variables non-negative

**Usage:**
```c
ralph_set_int_param(model, "detect_special", 1);  /* Enable for this model */
ralph_optimize(model);  /* Will use JVC if LAP structure detected */
```

**Benefits:**
- Transparent speedup: LAPs formulated as LPs get O(n³) JVC instead of simplex
- Disabled by default for fair benchmarking; enable per-model when desired

---

### 7. MIP with Assignment Subproblems
**Priority: Low** | **Complexity: High**

Specialized branch-and-bound for problems with assignment structure:
- Use LAP for LP relaxations
- Exploit problem structure in branching
- Tighter bounds from combinatorial analysis

---

## Performance Results (2026-01-29)

| Problem Type | Achieved | Notes |
|--------------|----------|-------|
| Dense n=500 | 2.6ms | Random uniform costs |
| Dense n=1000 | 13.9ms | - |
| Dense n=2000 | 66.5ms | - |
| Sparse n=500, 10% | 1.7ms | Native sparse JVC |
| Sparse n=1000, 10% | 6.7ms | Native sparse JVC |
| Sparse n=5000, 10% | 417ms | Native sparse JVC |
| Sparse n=1000, 50% | 9.7ms | Falls back to dense |

**Sparse vs Dense Crossover:**
- Native sparse faster when density < 20%
- Dense conversion faster when density > 30%
- Automatic threshold at 30% density

## Future Performance Targets

| Problem Type | Current | Target |
|--------------|---------|--------|
| Dense n=5000 | 900ms | 400ms |
| Sparse n=10000, 5% density | - | 200ms |
| Rectangular 1000×5000 | - | 200ms |

---

## Notes

- All new features should maintain 100% backward compatibility
- New APIs should follow existing naming conventions (`ralph_lap_*`)
- Performance-critical code should support both SIMD and scalar paths
- All features need comprehensive test coverage
