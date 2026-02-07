# Plan: Improve Ralph LP/MIP Solver Iteration Speed

## Executive Summary

This document outlines a comprehensive plan to improve Ralph's per-iteration performance to be competitive with GLPK. Based on profiling and analysis, the primary bottleneck is in LU operations (factorization and spike application), not the simplex algorithm itself.

## Current Performance Gap (Measured January 2026)

Benchmarked with `bench_vs_glpk`:

| Problem Size | Ralph Time | GLPK Time | Speedup | Ralph/iter | GLPK/iter | Iter Gap |
|--------------|------------|-----------|---------|------------|-----------|----------|
| 50×25 | 0.0002s | 0.0002s | 1.0× | 0.004ms | 0.003ms | 1.3× |
| 100×50 | 0.0008s | 0.0005s | 1.5× | 0.008ms | 0.005ms | 1.6× |
| 200×100 | 0.0056s | 0.0024s | 2.3× | 0.022ms | 0.008ms | 2.7× |
| 500×250 | 0.127s | 0.018s | **7.0×** | 0.136ms | 0.020ms | **6.8×** |
| 1000×500 | 1.49s | 0.12s | **12.0×** | 0.601ms | 0.044ms | **13.7×** |

**Key insight:** Both solvers take similar iterations (Ralph actually fewer due to Devex pricing), but the per-iteration cost gap grows dramatically with problem size - from 1.3× on small problems to 13.7× on larger ones. This indicates O(m²) or O(m³) operations in Ralph where GLPK achieves O(m × nnz).

## Profile Breakdown (Current Ralph)

| Component | % Time | Description |
|-----------|--------|-------------|
| `lu_factorize_sparse` | 42% | Initial/refactorization |
| `apply_ft_spikes_backward` | 27% | BTRAN update application |
| `apply_ft_spikes_forward` | 26% | FTRAN update application |
| Other | 5% | solve_L, solve_U, pricing, ratio test |

## How GLPK Achieves Better Performance

Based on GLPK source analysis (glpk-5.0/src/bflib/):

### 1. Efficient LU Representation

GLPK uses **Bartels-Golub-Reid (BGR)** factorization with:
- Compact storage for L and U factors
- Efficient eta-file representation
- Threshold pivoting with Markowitz criterion

### 2. Sparse Triangular Solves

GLPK's `btfint.c` implements:
- **Reach computation** - only process rows/columns affected by sparse RHS
- **Hyper-sparse mode** - switch between dense/sparse based on fill
- O(nnz) operations instead of O(m) for sparse RHS

### 3. Efficient Update Mechanism

GLPK's Forrest-Tomlin implementation:
- Minimal memory allocation during updates
- Efficient spike storage and application
- Smart refactorization trigger based on fill growth

### 4. Memory Management

- Pre-allocated work arrays
- Arena allocators for temporary storage
- Cache-friendly data layouts

---

## Improvement Plan

### Phase 1: Quick Wins (Expected 2-3× speedup)

#### 1.1 Eliminate Redundant Memory Allocations

**Current issue:** malloc/free calls within hot loops during spike application.

**Solution:**
```c
// Pre-allocate work vectors in LUFactorization struct
typedef struct {
    // ... existing fields ...
    double *work1, *work2;    // Pre-allocated scratch space
    int *iwork1, *iwork2;     // Integer scratch space
} LUFactorization;
```

**Files:** `src/lu.c`
**Expected improvement:** 1.2-1.5×

#### 1.2 Optimize Spike Application Loop

**Current issue:** Each spike application iterates through all elements.

**Solution:**
```c
// Current: O(sum of all spike nnz) per solve
// Improved: Skip spikes that don't affect active columns

// Track which positions have non-zeros
int *active_mask;  // Bitmap of active positions

for (int k = 0; k < num_spikes; k++) {
    int col = spike_col[k];
    if (!active_mask[col]) continue;  // Skip if column is zero
    // Apply spike
    // Update active_mask with new non-zeros
}
```

**Files:** `src/lu.c`
**Expected improvement:** 1.3-2×

#### 1.3 Spike Compaction

**Current issue:** With 3000 max_updates, we accumulate many spikes.

**Solution:**
```c
// Every N iterations, compact recent spikes into L/U
void lu_compact_spikes(LUFactorization *lu, int num_to_compact);
```

Periodically (every 100-200 updates) merge recent spikes back into the L/U factors. This trades slightly more work for fewer spikes to apply per solve.

**Files:** `src/lu.c`
**Expected improvement:** 1.3-1.5×

### Phase 2: Sparse Solves (Expected 3-5× speedup)

#### 2.1 Sparse FTRAN/BTRAN with Reach Computation

**Current issue:** Dense O(m) operations even for sparse RHS.

**Solution:** Implement Gilbert-Peierls sparse triangular solve:

```c
/* Compute "reach" - set of rows affected by sparse RHS */
void compute_reach(LUFactorization *lu, int *rhs_idx, int rhs_nnz,
                   int *reach, int *reach_len);

/* Sparse solve: only process rows in reach */
void lu_ftran_sparse(LUFactorization *lu,
                     double *rhs_val, int *rhs_idx, int rhs_nnz,
                     double *sol_val, int *sol_idx, int *sol_nnz);
```

**Algorithm:**
1. Compute reach (DFS on L/U dependency graph)
2. Topologically sort reached rows
3. Process only reached rows in order

**Files:** `src/lu.c`, new `src/lu_sparse.c`
**Expected improvement:** 3-5× for sparse RHS (typical in simplex)

#### 2.2 Hyper-Sparse Mode

**Current issue:** Always using same code path regardless of density.

**Solution:**
```c
#define HYPERSPARSE_THRESHOLD 0.1  // Switch at 10% density

void lu_solve(LUFactorization *lu, double *rhs, double *sol) {
    int nnz = count_nonzeros(rhs, lu->m);

    if (nnz < lu->m * HYPERSPARSE_THRESHOLD) {
        lu_solve_sparse(lu, rhs, sol);  // Sparse path
    } else {
        lu_solve_dense(lu, rhs, sol);   // Dense path
    }
}
```

**Files:** `src/lu.c`
**Expected improvement:** 1.5-2× on mixed problems

### Phase 3: LU Factorization Improvements (Expected 2-3× speedup)

#### 3.1 Symbolic/Numeric Separation

**Current issue:** Column ordering computed during numeric factorization.

**Solution:**
```c
// Symbolic analysis (reusable for similar structure)
LUSymbolic* lu_symbolic_analyze(const SparseMatrix *pattern);

// Numeric factorization (uses pre-computed structure)
int lu_numeric_factorize(LUFactorization *lu, const LUSymbolic *sym,
                         const SparseMatrix *B);
```

**Benefits:**
- Reuse symbolic analysis across refactorizations (basis structure often similar)
- Pre-allocate exact memory needed
- Better cache utilization

**Files:** `src/lu.c`, new `include/lu_symbolic.h`
**Expected improvement:** 1.5-2× for refactorization

#### 3.2 Better Fill-Reducing Ordering

**Current issue:** Simplified AMD may not minimize fill well.

**Solution:** Implement proper COLAMD or use structure-specific ordering for LP bases.

**LP-Specific insight:** LP bases often have structure (slack columns are identity columns). Exploit this:
```c
// Order singleton columns (identity) last
// Apply AMD only to non-singleton submatrix
```

**Files:** `src/lu.c`
**Expected improvement:** 1.2-1.5× from reduced fill

#### 3.3 Supernodal Factorization (Advanced)

**Current issue:** Column-by-column factorization.

**Solution:** Group columns with similar structure into "supernodes" and use dense BLAS.

This is a larger change requiring:
- Elimination tree construction
- Supernode detection
- Dense kernel integration

**Files:** `src/lu.c`, new `src/supernode.c`
**Expected improvement:** 2-4× for factorization (less for updates)

### Phase 4: Algorithmic Improvements (Expected 1.5-2× fewer iterations)

#### 4.1 Dual Simplex for All-<= Problems

**Current status:** Dual simplex exists but falls back to primal on fresh solves.

**Improvement:** Implement proper dual Phase 1 for all-<= LPs:
- Start with slack basis (dual-feasible for max problems)
- Use dual pivots to achieve primal feasibility

**Files:** `src/dual_simplex.c`
**Expected improvement:** 20-40% fewer iterations on suitable problems

#### 4.2 True Steepest Edge Pricing

**Current status:** Devex (approximate steepest edge).

**Improvement:** Implement exact steepest edge with incremental weight updates.

**Trade-off:** More work per iteration but fewer iterations overall.

**Files:** `src/simplex.c`
**Expected improvement:** 10-20% fewer iterations

#### 4.3 Bound-Flipping Ratio Test (BFRT)

**Current status:** Standard Harris ratio test.

**Improvement:** Allow variables to flip bounds during ratio test:
```c
// When a variable would go to its opposite bound, flip it
// without a full pivot - reduces basis changes
```

**Files:** `src/simplex.c`
**Expected improvement:** 5-15% fewer iterations on degenerate problems

---

## Implementation Roadmap

### Week 1-2: Quick Wins
- [ ] Pre-allocate work vectors in LU struct
- [ ] Optimize spike application with active set tracking
- [ ] Implement spike compaction (every 200 updates)
- [ ] Run benchmarks, verify correctness

**Milestone:** 2× speedup on 500×250

### Week 3-4: Sparse Solves
- [ ] Implement reach computation for L
- [ ] Implement reach computation for U
- [ ] Add sparse FTRAN/BTRAN
- [ ] Add hyper-sparse mode switching
- [ ] Integrate with simplex iteration

**Milestone:** 5× speedup on 500×250

### Week 5-6: LU Improvements
- [ ] Separate symbolic and numeric phases
- [ ] Improve column ordering for LP bases
- [ ] Optimize refactorization triggers

**Milestone:** 8× speedup on 500×250 (within 6× of GLPK)

### Week 7-8: Algorithm Improvements
- [ ] Dual Phase 1 for all-<= problems
- [ ] Exact steepest edge pricing
- [ ] BFRT implementation

**Milestone:** 10× speedup, within 5× of GLPK

---

## Benchmarking Strategy

### Benchmark Suite

Created in `ralph/benchmarks/`:
- `bench_vs_glpk.c` - Direct comparison with GLPK
- `bench_profile.c` - Detailed profiling of components
- `bench_netlib.c` - Standard Netlib problems (optional)

### Key Metrics

1. **Total solve time** - Primary metric
2. **Iteration count** - Algorithm efficiency
3. **Time per iteration** - Implementation efficiency
4. **LU factorization time** - Factorization performance
5. **FTRAN/BTRAN time** - Solve performance
6. **Update time** - Spike application performance

### Comparison Points

| Problem | Ralph Current | Ralph Target | GLPK |
|---------|---------------|--------------|------|
| 100×50 | 0.8ms | 0.5ms | 0.5ms |
| 200×100 | 5.6ms | 3ms | 2.4ms |
| 500×250 | 127ms | 30ms | 18ms |
| 1000×500 | 1.49s | 0.25s | 0.12s |

**Goal:** Within 2× of GLPK for medium problems, within 3× for large problems

---

## Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| Sparse solves | Medium | Fall back to dense if nnz > m/4 |
| Spike compaction | Low | Conservative threshold, test extensively |
| Symbolic separation | Low | Clear interface, gradual rollout |
| Dual Phase 1 | Medium | Keep primal fallback, extensive testing |

---

## References

### GLPK Source Files (Key)
- `glpk-5.0/src/bflib/btfint.c` - Basis factorization interface
- `glpk-5.0/src/bflib/fhvint.c` - FHV (Forrest-Tomlin) implementation
- `glpk-5.0/src/bflib/lufint.c` - LU factorization
- `glpk-5.0/src/simplex/spxlp.c` - Simplex LP driver

### Papers
1. Gilbert & Peierls (1988) - "Sparse Partial Pivoting"
2. Duff, Erisman, Reid (1986) - "Direct Methods for Sparse Matrices"
3. Suhl & Suhl (1990) - "Computing Sparse LU for LP"

### Tools
```bash
# Install GLPK for comparison
brew install glpk

# Build Ralph benchmarks
cd ralph && make benchmarks

# Run comparison
./bench_vs_glpk

# Profile with instruments (macOS)
instruments -t "Time Profiler" ./bench_vs_glpk
```
