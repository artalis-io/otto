# Dual Simplex Numerical Stability Improvements

## Current State

The dual simplex implementation works correctly for small problems but has numerical stability issues on larger problems (>100 constraints), causing it to fall back to primal simplex.

### Benchmark Results (Current)

| Size | Primal Time | Dual Time | Notes |
|------|-------------|-----------|-------|
| 50×25 | 0.000s | 0.005s | Works |
| 100×50 | 0.001s | 0.099s | Falls back after ~100 dual iters |
| 200×100 | 0.002s | 0.885s | Falls back after ~1000 dual iters |
| 500×250 | 0.041s | 19.6s | Falls back after ~2600 dual iters |

### Root Causes

1. **Reduced Cost Drift**: Incremental updates to reduced costs accumulate errors
2. **Bound-Flip Start**: Starting all variables at upper bound creates highly primal-infeasible starting point
3. **Pivot Selection**: Dual ratio test doesn't always select numerically stable pivots
4. **No Perturbation**: Degeneracy causes cycling-like behavior

---

## Proposed Improvements (Prioritized)

### Phase 1: Quick Wins (Low Risk)

#### 1.1 Steepest Edge Pricing for Dual
**File**: `dual_simplex.c`
**Effort**: Medium

Currently using simple "most infeasible" leaving variable selection. Implement Dual Steepest Edge (DSE) pricing:

```c
/* Select leaving variable with largest weighted infeasibility */
double weight = infeas * infeas / dse_weight[k];
if (weight > max_weight) {
    leaving = k;
    max_weight = weight;
}
```

Benefits:
- Fewer iterations (typically 30-50% reduction)
- More numerically stable pivot choices
- Well-documented algorithm (Forrest & Goldfarb)

#### 1.2 Bound Perturbation
**File**: `dual_simplex.c`
**Effort**: Low

Add small perturbations to bounds to avoid degeneracy:

```c
/* In make_dual_feasible() */
double eps = 1e-6 * (1.0 + fabs(ub));
tab->ub_ext[j] += eps * (j % 7 + 1);  /* Pseudo-random perturbation */
```

Then remove perturbations at the end. This breaks ties and prevents cycling.

#### 1.3 Harris Ratio Test for Dual
**File**: `dual_simplex.c`
**Effort**: Low

Current ratio test selects minimum ratio. Harris test allows near-minimum ratios for better numerical stability:

```c
/* Harris: accept any ratio within tolerance of minimum */
double harris_tol = 1e-7;
double threshold = min_ratio * (1.0 + harris_tol) + harris_tol;
/* Select pivot with largest |alpha| among candidates with ratio <= threshold */
```

### Phase 2: Core Stability (Medium Risk)

#### 2.1 Refactorization Frequency
**File**: `dual_simplex.c`
**Effort**: Low

Current: Refactorize based on eta count.
Proposed: Also refactorize based on condition number estimate:

```c
if (lu_needs_refactorization(tab->lu) ||
    tab->lu->cond_estimate > 1e10 ||
    iter % 50 == 0) {
    tableau_refactorize(tab);
}
```

#### 2.2 BFRT (Bound-Flipping Ratio Test)
**File**: `dual_simplex.c`
**Effort**: Medium

When a variable wants to flip bounds during ratio test, do the flip instead of a full pivot:

```c
/* If ratio test indicates bound flip is better than pivot */
if (can_flip && flip_improves_dual) {
    tab->x[j] = other_bound;
    tab->var_status[j] = new_status;
    continue;  /* No basis change needed */
}
```

This avoids expensive pivots for degenerate steps.

#### 2.3 Warm Start from Primal Basis
**File**: `dual_simplex.c`, `ralph.c`
**Effort**: Medium

Instead of starting from scratch with slack basis:

```c
/* If we have a primal feasible basis, use it */
if (solver->tableau && is_primal_feasible(solver->tableau)) {
    /* Just need to achieve dual feasibility */
    make_dual_feasible_from_primal(tab);
    /* Run dual Phase 2 */
}
```

This is useful for re-optimization scenarios (MIP, sensitivity).

### Phase 3: Advanced Techniques (Higher Risk)

#### 3.1 LU Stability Improvements
**File**: `lu.c`
**Effort**: High

Implement threshold rook pivoting for better stability:

```c
/* Select pivot that's large in both row and column */
for each candidate (i,j):
    if |a[i][j]| >= threshold * max_in_row[i] &&
       |a[i][j]| >= threshold * max_in_col[j]:
        consider as pivot
```

#### 3.2 Iterative Refinement for Dual
**File**: `dual_simplex.c`
**Effort**: Medium

After computing reduced costs, refine them:

```c
/* Compute residual: r = c - A'y - rc (should be 0) */
/* If ||r|| > tol, solve for correction */
```

#### 3.3 Boxed Dual Simplex
**File**: `dual_simplex.c`
**Effort**: High

Full implementation of dual simplex with bounded variables (not just bound-flipping heuristic). This requires:
- Proper dual Phase 1 with artificial variables
- Dual pricing with bound awareness
- Proper ratio test for bounded variables

---

## Implementation Order

| Priority | Item | Impact | Risk | Effort |
|----------|------|--------|------|--------|
| 1 | Harris Ratio Test | Medium | Low | Low |
| 2 | Bound Perturbation | Medium | Low | Low |
| 3 | Refactorization Frequency | Medium | Low | Low |
| 4 | Steepest Edge Pricing | High | Low | Medium |
| 5 | BFRT | High | Medium | Medium |
| 6 | Warm Start | Medium | Medium | Medium |
| 7 | LU Improvements | Medium | Medium | High |
| 8 | Iterative Refinement | Low | Low | Medium |
| 9 | Boxed Dual Simplex | High | High | High |

---

## Success Metrics

1. **Iteration Reduction**: Target 50% fewer iterations before fallback
2. **Fallback Rate**: Target <10% fallback for problems up to 500×250
3. **Correctness**: All existing tests pass
4. **Performance**: Dual at most 2x slower than primal (when not falling back)

---

## Testing Strategy

1. Create benchmark suite with varying:
   - Problem sizes (50-1000 constraints)
   - Density (10%, 30%, 50%)
   - Degeneracy level (# of constraints at equality)
   - Bound types (all bounded, some unbounded, mixed)

2. Track metrics:
   - Iterations to optimality (or fallback)
   - Dual feasibility violations per iteration
   - Condition number of basis
   - Final objective accuracy

3. Compare against:
   - Current primal simplex
   - GLPK dual simplex
   - Reference implementations
