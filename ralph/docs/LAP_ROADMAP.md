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
- [x] Optional ε-scaling auction for tie-breaking

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
