# Revised Simplex Method Implementation

## Overview

Ralph implements the Revised Simplex method, which is more efficient than the standard tableau method for large sparse problems. Instead of maintaining the full simplex tableau, we maintain the LU factorization of the basis matrix.

## Mathematical Foundation

### Standard Form LP

```
minimize    c'x
subject to  Ax = b
            l ≤ x ≤ u
```

### Basis Representation

At each iteration, we partition variables into:
- **Basic variables (B):** m variables that form a nonsingular basis matrix
- **Nonbasic variables (N):** n-m variables at their bounds

The system Ax = b becomes:
```
B × xB + N × xN = b
xB = B⁻¹(b - N × xN)
```

## Implementation Details

### Pricing (Variable Selection)

**File:** `simplex.c`, functions `pricing_dantzig()`, `pricing_steepest_edge()`

**Reduced Cost Computation:**
```c
// Compute dual values: y = B⁻ᵀ × cB
lu_solve_transpose(lu, c_B, y);

// Compute reduced costs: rc[j] = c[j] - y'A[j]
for (j in nonbasic) {
    rc[j] = c[j] - dot(y, A[:, j]);
}
```

**Selection Criteria:**
- **Dantzig:** Choose j with most negative rc[j]
- **Steepest Edge:** Choose j with max |rc[j]| / √γ[j] where γ is the edge weight

### Ratio Test (Row Selection)

**File:** `simplex.c`, function `ratio_test_harris()`

**Direction Computation:**
```c
// d = B⁻¹ × A[entering]
sparse_get_column(A, entering, work);
lu_solve(lu, work, d);
```

**Harris Two-Pass Ratio Test:**
```c
// Pass 1: Find θmax with tolerance
θmax = ∞
for (k = 0; k < m; k++) {
    if (d[k] > tol) {
        ratio = (x[basis[k]] - lb[basis[k]] + feas_tol) / d[k];
        θmax = min(θmax, ratio);
    }
}

// Pass 2: Select leaving with largest |d[k]| among candidates
leaving = -1;
best_pivot = 0;
for (k = 0; k < m; k++) {
    if (d[k] > tol) {
        ratio = (x[basis[k]] - lb[basis[k]]) / d[k];
        if (ratio <= θmax && |d[k]| > best_pivot) {
            leaving = k;
            best_pivot = |d[k]|;
        }
    }
}
```

### Pivot Operation

**File:** `simplex.c`, function `simplex_pivot()`

```c
// Update entering variable
x[entering] += θ;

// Update basic variables
for (k = 0; k < m; k++) {
    x[basis[k]] -= θ × d[k];
}

// Update basis
basis[leaving] = entering;
basis_pos[entering] = leaving;
basis_pos[old_leaving] = -1;

// Update LU factorization
lu_update(lu, leaving, A[:, entering]);
```

## Handling Different Constraint Types

### Constraint Normalization

Before building the tableau, constraints are normalized to have non-negative RHS:

```c
for (i = 0; i < num_cons; i++) {
    if (b[i] < 0) {
        // Multiply row by -1
        A[i, :] *= -1;
        b[i] *= -1;
        // Flip sense: L ↔ G
    }
}
```

### Adding Slack/Artificial Variables

| Constraint Type | Variables Added | Initial Basis |
|-----------------|-----------------|---------------|
| ≤ (after norm)  | slack (+1)      | slack |
| ≥ (after norm)  | surplus (-1), artificial (+1) | artificial |
| = (after norm)  | artificial (+1) | artificial |

### Big-M Method

Artificial variables get cost M = 10⁸. After optimization:
- If artificial value > 0 → Problem is infeasible
- If artificial value = 0 → Original problem is feasible

## Phase 1 / Phase 2

**Phase 1:** Check if initial basis is feasible
- If basic variables violate bounds, use dual simplex pivots to restore feasibility

**Phase 2:** Optimize the objective
- Standard primal simplex iterations
- Terminate when all reduced costs are non-negative (for minimization)

## Numerical Stability

### Refactorization

After `max_updates` (default 50) eta updates, refactorize from scratch:
```c
if (lu_needs_refactorization(lu)) {
    B = build_basis_matrix(tab);
    lu_factorize(lu, B);
}
```

### Tolerances

```c
#define RALPH_ZERO_TOL    1e-12  // Treat as zero
#define RALPH_PIVOT_TOL   1e-8   // Minimum pivot magnitude
#define RALPH_FEAS_TOL    1e-6   // Feasibility tolerance
#define RALPH_OPT_TOL     1e-6   // Optimality tolerance
```

## Example Trace

```
Problem: min -x - y, s.t. x + y ≤ 4, 2x + y ≤ 6

Initial:
  basis = [s1, s2], x = [0, 0, 4, 6], obj = 0
  rc = [-1, -1, 0, 0]

Iteration 1:
  entering = 0 (x), rc[0] = -1
  d = B⁻¹ × [1, 2] = [1, 2]
  ratios: s1: 4/1 = 4, s2: 6/2 = 3
  leaving = 1 (s2), θ = 3
  x = [3, 0, 1, 0], obj = -3

Iteration 2:
  rc = [0, -1, 0, 0.5]
  entering = 1 (y)
  d = [1, -1]
  ratios: s1: 1/1 = 1
  leaving = 0 (s1), θ = 1
  x = [2, 2, 0, 0], obj = -4

Optimal: x = 2, y = 2, obj = -4
```
