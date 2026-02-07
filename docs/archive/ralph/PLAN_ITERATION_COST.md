# Plan: Reduce Per-Iteration Cost

## Current Performance Gap

| Size | Ralph | GLPK | Gap |
|------|-------|------|-----|
| 200×100 | 70ms | 9ms | 8x |
| 500×200 | 6.5s | 60ms | 111x |

The gap grows superlinearly with problem size, indicating O(m²) or O(m³) operations where GLPK achieves O(m × nnz).

---

## Per-Iteration Breakdown

Each simplex iteration performs:

| Operation | Current Cost | Optimal Cost | Priority |
|-----------|--------------|--------------|----------|
| BTRAN (y = c_B × B⁻¹) | O(m²) dense | O(nnz_L + nnz_U) | **Critical** |
| Pricing (select entering) | O(n × m) | O(n × avg_nnz) | High |
| FTRAN (α = B⁻¹ × a_j) | O(m²) dense | O(nnz_L + nnz_U) | **Critical** |
| Ratio test | O(m) | O(m) | OK |
| Basis update | O(m) | O(nnz_spike) | OK |

---

## Phase 1: Sparse BTRAN/FTRAN (Critical)

### Problem
Current `lu_solve` and `lu_solve_transpose` use dense O(m) vectors even when RHS is sparse.

### Solution: Exploit RHS Sparsity

```c
/* Track nonzero pattern during solves */
typedef struct {
    double *x;      /* Dense values */
    int *nz_idx;    /* Indices of nonzeros */
    int nnz;        /* Count of nonzeros */
} SparseVector;
```

**FTRAN optimization:**
- Input: sparse column a_j (typically 3-10 nonzeros)
- L solve: only update rows reachable from nonzero RHS entries
- U solve: only compute entries that affect the result

**BTRAN optimization:**
- Input: c_B (often mostly zeros - slack costs are 0)
- Transpose solve exploits sparsity of cost vector

### Implementation

1. Add `lu_solve_sparse_to_sparse()` that returns sparse result
2. Compute "reach" of nonzero pattern through L and U
3. Only perform operations on reached rows/columns

**Expected improvement:** 5-10x for sparse RHS vectors

---

## Phase 2: Hyper-Sparse Pricing

### Problem
Current pricing scans all n-m nonbasic variables, computing α_j for each.

### Solution: Partial Pricing + PRICE

```c
/* Only scan subset of variables per iteration */
int partial_pricing(SimplexTableau *tab, int *entering) {
    int chunk_size = (tab->n - tab->m) / 10;  /* 10% per iteration */
    int start = tab->partial_price_pos;

    /* Scan chunk, wrap around */
    for (int k = 0; k < chunk_size; k++) {
        int j = tab->nonbasis[(start + k) % (tab->n - tab->m)];
        /* Check reduced cost */
    }
    tab->partial_price_pos = (start + chunk_size) % (tab->n - tab->m);
}
```

### PRICE Operation (Sparse Row-wise)

Instead of computing full pivot row, use:
```c
/* Compute reduced cost for single variable */
double compute_rc_sparse(SimplexTableau *tab, int j) {
    /* rc[j] = c[j] - y' * A_j */
    double rc = tab->c_ext[j];
    for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j+1]; p++) {
        rc -= tab->y[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
    }
    return rc;
}
```

**Expected improvement:** 2-3x for pricing phase

---

## Phase 3: Incremental Updates

### 3.1 Dual Steepest Edge Weights

Maintain edge weights incrementally instead of recomputing:
```c
/* After pivot, update weights */
for (int k = 0; k < tab->m; k++) {
    if (k != leaving_pos) {
        double tau = pivot_row[k] / pivot;
        tab->dse_weights[k] += tau * tau * tab->dse_weights[leaving_pos]
                              - 2 * tau * /* correction term */;
    }
}
tab->dse_weights[entering_pos] = /* new weight */;
```

### 3.2 Reduced Cost Updates

Already partially implemented. Ensure it's used consistently:
```c
/* After pivot: rc_new[j] = rc_old[j] - (rc[enter]/pivot) * alpha_j */
```

**Expected improvement:** 1.5-2x for degenerate problems

---

## Phase 4: Memory Access Optimization

### 4.1 Cache-Friendly Data Layout

```c
/* Pack frequently accessed data together */
typedef struct {
    double value;
    int index;
} PackedEntry;

/* Process in cache-line sized chunks */
#define CACHE_LINE 64
```

### 4.2 Avoid Redundant Allocations

```c
/* Reuse work vectors instead of malloc/free per iteration */
typedef struct {
    double *ftran_result;   /* Preallocated */
    double *btran_result;   /* Preallocated */
    double *pivot_row;      /* Preallocated */
    int *sparse_idx;        /* For sparse operations */
} SimplexWorkspace;
```

**Expected improvement:** 1.2-1.5x

---

## Implementation Order

### Week 1: Sparse Triangular Solves
1. [ ] Implement reach computation for L
2. [ ] Implement reach computation for U
3. [ ] Add `lu_ftran_sparse()` returning sparse vector
4. [ ] Add `lu_btran_sparse()` for transpose
5. [ ] Integrate into simplex iteration

### Week 2: Pricing Improvements
1. [ ] Add partial pricing with configurable chunk size
2. [ ] Implement sparse PRICE operation
3. [ ] Add multiple pricing strategy (scan top-k candidates)

### Week 3: Incremental Updates
1. [ ] Implement DSE weight maintenance
2. [ ] Verify incremental RC updates work correctly
3. [ ] Add periodic full recomputation for numerical stability

### Week 4: Polish
1. [ ] Profile to find remaining hotspots
2. [ ] Optimize memory allocation patterns
3. [ ] Add SIMD for dense vector operations where applicable

---

## Verification

After each phase, verify:
1. All 45 tests still pass
2. Solutions match GLPK exactly
3. Measure speedup on 500×200 benchmark

**Target:** Reduce 500×200 from 6.5s to <0.5s (13x improvement)

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/lu.c` | Sparse FTRAN/BTRAN with reach |
| `src/simplex.c` | Partial pricing, DSE weights |
| `include/lp.h` | SparseVector struct, workspace |
| `src/pricing.c` | New file for pricing strategies |

---

## Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| Sparse solves | Medium | Fall back to dense if nnz > m/4 |
| Partial pricing | Low | Can disable and use full scan |
| DSE weights | Medium | Periodic recomputation |
| Incremental RC | Low | Already partially implemented |
