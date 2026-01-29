# LAP Solver Roadmap

Future improvements and enhancements for the Ralph LAP solver.

## Implemented Features

- [x] Dense JVC algorithm (O(n³))
- [x] Minimization and maximization
- [x] Sparse LAP (via dense conversion)
- [x] LP-based solver (for verification)
- [x] Workspace reuse API
- [x] SIMD optimizations
- [x] OpenMP parallelization (optional)
- [x] Rectangular LAP (m × n problems)
- [x] Runtime parallel enable/disable

---

## Planned Improvements

### 1. Sparse-Native JVC Implementation
**Priority: High** | **Complexity: Medium**

Currently sparse problems are converted to dense format. A true sparse implementation would:
- Use CSR format directly in all phases
- Only iterate over finite-cost edges
- Skip forbidden assignments without memory access

**Benefits:**
- 10-100x faster for sparse problems (e.g., 10% density)
- Lower memory usage (O(nnz) vs O(n²))
- Essential for large matching problems

**API:**
```c
RalphLapStatus ralph_lap_solve_sparse_native(
    int n,
    int nnz,
    const int *row_ptr,
    const int *col_idx,
    const double *values,
    RalphLapObjective objective,
    int *row_sol,
    double *total_cost
);
```

---

### 2. Incremental Updates / Warm Start
**Priority: Medium** | **Complexity: High**

For applications that solve similar LAPs repeatedly (tracking, auction algorithms):
- Reuse dual variables from previous solution
- Update only affected parts when costs change
- Support single-cell and batch updates

**Benefits:**
- Much faster for iterative applications
- Essential for real-time tracking systems

**API:**
```c
/* Initialize from previous solution */
RalphLapStatus ralph_lap_warm_start(
    RalphLapWorkspace *ws,
    int n,
    const double *u,      /* Previous row duals */
    const double *v       /* Previous column duals */
);

/* Update single cost and re-solve */
RalphLapStatus ralph_lap_update_cost(
    RalphLapWorkspace *ws,
    int row,
    int col,
    double new_cost,
    int *row_sol,
    double *total_cost
);
```

---

### 3. Cost Matrix Callbacks
**Priority: Medium** | **Complexity: Medium**

For very large problems, avoid storing full n² matrix:
- Compute costs on-demand via callback
- Useful when cost function is simple (e.g., Euclidean distance)
- Can integrate with external data sources

**Benefits:**
- O(n) memory instead of O(n²)
- Enables problems too large to fit in memory

**API:**
```c
typedef double (*RalphLapCostFn)(int i, int j, void *user_data);

RalphLapStatus ralph_lap_solve_callback(
    int n,
    RalphLapCostFn cost_fn,
    void *user_data,
    RalphLapObjective objective,
    int *row_sol,
    double *total_cost
);
```

---

### 4. ε-Scaling Auction Algorithm
**Priority: Low** | **Complexity: High**

Alternative algorithm with guaranteed polynomial convergence:
- Start with large ε, refine progressively
- More predictable iteration counts
- Better for certain problem structures

**Benefits:**
- Guaranteed O(n³ log(nC)) complexity
- More robust on degenerate problems

---

### 5. k-Best Assignments
**Priority: Low** | **Complexity: High**

Find the k best (non-overlapping) assignments:
- Uses Murty's algorithm
- Returns ranked list of solutions

**Benefits:**
- Useful for multi-hypothesis tracking
- Provides solution diversity

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
```

---

## Ralph Solver Integration

### 6. Presolve Detection
**Priority: Medium** | **Complexity: Medium**

Detect assignment structure in general LPs and use JVC:
- Recognize totally unimodular constraint matrices
- Identify pure assignment problems
- Automatic algorithm selection

---

### 7. MIP with Assignment Subproblems
**Priority: Low** | **Complexity: High**

Specialized branch-and-bound for problems with assignment structure:
- Use LAP for LP relaxations
- Exploit problem structure in branching
- Tighter bounds from combinatorial analysis

---

## Performance Targets

| Problem Type | Current | Target |
|--------------|---------|--------|
| Dense n=1000 | 10ms | 5ms |
| Dense n=5000 | 900ms | 400ms |
| Sparse n=10000, 1% density | N/A | 50ms |
| Rectangular 1000×5000 | 900ms | 200ms |

---

## Notes

- All new features should maintain 100% backward compatibility
- New APIs should follow existing naming conventions (`ralph_lap_*`)
- Performance-critical code should support both SIMD and scalar paths
- All features need comprehensive test coverage
