# Ralph LP Solver Internals

This document provides detailed technical documentation of the Ralph LP/MIP solver internals for future reference and development.

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Solve Pipeline](#solve-pipeline)
3. [Component Deep Dives](#component-deep-dives)
4. [Key Data Structures](#key-data-structures)
5. [Performance Findings](#performance-findings)
6. [Known Limitations](#known-limitations)
7. [Optimization History](#optimization-history)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     User API (ralph.c)                          │
│  ralph_create() → ralph_add_var() → ralph_add_constraint()      │
│  → ralph_optimize() → ralph_get_objval()                        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Model Building (model.c)                      │
│  LPModel: variables, bounds, constraints, objective             │
│  Sparse matrix in CSC format (column-major)                     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Presolve (presolve.c)                         │
│  [OPTIONAL - disabled by default]                               │
│  Reduces problem size before solving                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Simplex Solver (simplex.c)                         │
│                                                                 │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐         │
│  │   Phase 1   │───▶│   Phase 2   │───▶│  Solution   │         │
│  │  (Big-M)    │    │ (Optimize)  │    │  Recovery   │         │
│  └─────────────┘    └─────────────┘    └─────────────┘         │
│         │                  │                                    │
│         ▼                  ▼                                    │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Simplex Iteration Loop                      │   │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐     │   │
│  │  │ Pricing │─▶│  Ratio  │─▶│  Pivot  │─▶│   LU    │     │   │
│  │  │         │  │  Test   │  │         │  │ Update  │     │   │
│  │  └─────────┘  └─────────┘  └─────────┘  └─────────┘     │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│            LU Factorization (lu.c, lu_sparse.c)                 │
│  Maintains B = LU for basis matrix                              │
│  Supports updates via eta-file (rank-1 updates)                 │
└─────────────────────────────────────────────────────────────────┘
```

### For MIP Problems:

```
┌─────────────────────────────────────────────────────────────────┐
│                   MIP Solver (mip.c)                            │
│  Coordinates LP relaxation + Branch & Bound                     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│              Branch & Bound (branch_bound.c)                    │
│  Node selection, branching, incumbent management                │
│  Uses DUAL SIMPLEX for warm-started resolves                    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                Cut Generation (cuts.c)                          │
│  Gomory mixed-integer cuts                                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## Solve Pipeline

### 1. Model Construction

**File:** `model.c`

```c
LPModel* lp_model_create();
void lp_model_add_var(model, lb, ub, obj_coef, type);
void lp_model_add_constraint(model, nnz, indices, values, sense, rhs);
void lp_model_finalize(model);  // Builds CSC matrix
```

- Variables stored with bounds `lb[j]`, `ub[j]`, objective `c[j]`, type `var_type[j]`
- Constraints stored in CSC sparse matrix `A` with RHS `b[i]` and sense `sense[i]` ('L', 'G', 'E')
- `lp_model_finalize()` converts triplet storage to CSC format

### 2. Presolve (Optional)

**File:** `presolve.c`
**Default:** Disabled (adds overhead on random LPs)

Presolve techniques implemented:
1. **Remove fixed variables** - Variables where `lb[j] == ub[j]`
2. **Remove empty rows** - Constraints with no non-zeros
3. **Remove empty columns** - Variables with no coefficients
4. **Singleton rows** - Constraints with exactly one variable
5. **Singleton columns** - Variables appearing in exactly one constraint
6. **Forcing constraints** - Constraints that fix all variables
7. **Bound tightening** - Derive tighter bounds from constraints

**Key optimization (commit 82b6f80):** Changed from O(m×n²) to O(m×n) by extracting rows once instead of calling `sparse_get_element()` per element.

### 3. Tableau Creation

**File:** `simplex.c`, function `tableau_create()`

Extends the model with auxiliary variables:
- **<= constraint:** Add slack variable (basic, coefficient +1)
- **>= constraint:** Add surplus (-1) + artificial (+1, basic)
- **= constraint:** Add artificial variable (basic, coefficient +1)

Creates extended matrix `A_ext` with dimensions `m × (n + num_aux_vars)`.

### 4. Phase 1: Finding Initial BFS

**File:** `simplex.c`, function `simplex_phase1()`

Uses **Big-M method** with M = 1e8:
- Artificial variables get objective coefficient M (for minimization)
- Phase 1 drives artificial variables to zero
- If any artificial variable remains non-zero → INFEASIBLE

**Note:** This is less efficient than the two-phase method or crash procedures. GLPK uses more sophisticated initialization which contributes to their iteration advantage.

### 5. Phase 2: Optimization

**File:** `simplex.c`, function `simplex_phase2()`

Main iteration loop:

```
while (not optimal):
    1. Compute reduced costs: rc = c - c_B * B^{-1} * A
    2. PRICING: Select entering variable j with rc[j] < 0 (for min)
    3. Compute pivot column: d = B^{-1} * a_j
    4. RATIO TEST: Select leaving variable i
    5. PIVOT: Update basis, solution, and LU factorization
```

### 6. Pricing Strategies

**File:** `simplex.c`

| Strategy | Function | Description |
|----------|----------|-------------|
| Dantzig (0) | `pricing_dantzig()` | Most negative reduced cost |
| Steepest Edge (1) | `pricing_steepest_edge()` | `max |rc|/||d||` (exact, expensive) |
| **Devex (2)** | `pricing_devex()` | Approximate steepest edge (default) |

**Devex Implementation Details:**

```c
// Selection: maximize rc²/weight
for each nonbasic j:
    ratio = rc[j]² / se_weights[j]
    if ratio > best: entering = j

// Update (after pivot):
for each nonbasic j:
    alpha_j = pivot_row · a_j
    new_weight = (alpha_j² × gamma_e) / pivot²
    se_weights[j] = max(se_weights[j], new_weight)

// Periodic reset every n iterations (prevents drift)
if devex_refcount >= n:
    reset all weights to 1.0
```

**Key fix (commit d925029):** Pivot row must be computed BEFORE LU update (using old basis), not after.

### 7. Ratio Test

**File:** `simplex.c`

| Method | Function | Description |
|--------|----------|-------------|
| Bland's Rule | `ratio_test_bland()` | Anti-cycling, uses smallest index |
| Harris's Rule | `ratio_test_harris()` | Two-phase with tolerance (default) |

**Harris's Rule (current default):**
1. Phase 1: Find maximum step `theta_max` allowing small infeasibility
2. Phase 2: Among candidates within `theta_max`, pick largest pivot

**Cycling Detection:**
- Track consecutive degenerate pivots (`theta < 1e-3`)
- Switch to Bland's rule after `DEGEN_THRESHOLD` (50) consecutive degenerate pivots
- Switch back after `NON_DEGEN_THRESHOLD` (20) non-degenerate pivots

### 8. LU Factorization

**Files:** `lu.c` (dense operations), `lu_sparse.c` (sparse factorization)

**Factorization:**
- Sparse LU with Markowitz pivot selection (minimize fill-in)
- Threshold pivoting for numerical stability
- Partial pivoting with row permutations

**Updates (Eta-File Method):**
```c
// After each pivot, store eta vector (sparse):
eta_indices[k], eta_values[k], eta_nnz[k]

// Apply updates via forward/backward substitution
// Refactorize when:
//   - num_updates >= max_updates (100)
//   - growth_factor > 1e6
```

**Key optimizations:**
- Sparse eta storage (commit cef048f)
- Use row lists for U extraction (commit 5a41745)
- Increased refactorization thresholds (commit bd9176a)

---

## Key Data Structures

### SimplexTableau (simplex.c)

```c
typedef struct {
    int m, n;                    // Dimensions (m constraints, n variables)
    SparseMatrix *A_ext;         // Extended constraint matrix
    double *c_ext;               // Extended objective
    double *lb_ext, *ub_ext;     // Extended bounds

    int *basis;                  // basis[i] = index of i-th basic variable
    int *var_status;             // BASIC, NONBASIC_LOWER, NONBASIC_UPPER, NONBASIC_FREE

    double *x;                   // Current solution
    double *rc;                  // Reduced costs

    LUFactorization *lu;         // LU factors of basis matrix

    // Devex pricing
    double *se_weights;          // Steepest edge weights
    int devex_refcount;          // Pivots since last reset
    int use_steepest_edge;       // Enable Devex pricing

    // Work vectors
    double *work1, *work2;       // Temporary storage
} SimplexTableau;
```

### LUFactorization (lp.h)

```c
typedef struct {
    int m;                       // Dimension

    // Permutations
    int *perm;                   // Row permutation
    int *iperm;                  // Inverse row permutation

    // L factor (stored by rows)
    int *L_colptr, *L_rowidx;
    double *L_values;

    // U factor (stored by columns)
    int *U_colptr, *U_rowidx;
    double *U_values;

    // Eta-file (sparse storage)
    int eta_capacity;
    int num_eta;
    int *eta_col;                // Column index for each eta
    int **eta_indices;           // Row indices per eta
    double **eta_values;         // Values per eta
    int *eta_nnz;                // Non-zeros per eta

    // Refactorization control
    int num_updates;
    int max_updates;             // 100
    double growth_factor;        // Triggers refac if > 1e6
} LUFactorization;
```

### SparseMatrix (sparse.h)

```c
typedef struct {
    int nrows, ncols, nnz;
    int *colptr;                 // Column pointers (CSC format)
    int *rowidx;                 // Row indices
    double *values;              // Non-zero values
} SparseMatrix;
```

---

## Performance Findings

### Benchmark: 1000×500 LP, 30% density

| Solver | Iterations | Time | Per-Iteration |
|--------|-----------|------|---------------|
| GLPK | 305 | 0.055s | 180µs |
| Ralph | 1098 | 1.05s | 954µs |
| **Ratio** | **3.6×** | **19×** | **5.3×** |

### Iteration Count Analysis

**Devex IS helping:**
- 200×100: Ralph 69 iterations vs GLPK 75 (0.92× - Ralph wins!)
- With Dantzig: same problem needs 136 iterations (2× worse)

**Why 3.6× more iterations on large problems:**

1. **Algorithm difference:** GLPK uses dual simplex for <= constraints. Dual simplex starts with a dual-feasible (optimal) basis and drives primal infeasibility to zero, which is often more efficient.

2. **Initial basis:** Ralph uses Big-M method with artificial variables. GLPK uses sophisticated crash procedures to find a better starting basis.

3. **Degeneracy handling:** Different cycling prevention strategies may affect iteration counts.

### Per-Iteration Cost Breakdown (profiling)

```
Function                    % Time    Calls
─────────────────────────────────────────────
lu_factorize_sparse         60.8%     11
lu_solve_transpose          6.9%      2207
sparse_matvec_transpose_add 6.9%      1109
simplex_pivot               5.4%      1098
apply_eta_forward           2.3%      2230
```

**Main bottleneck:** LU factorization (even at 11 refactorizations for 1098 iterations).

---

## Known Limitations

1. **No dual simplex for main solve** - Only primal simplex available. Dual simplex exists but only used for MIP re-optimization.

2. **Big-M method for Phase 1** - Less efficient than two-phase or crash procedures.

3. **No column generation** - Not suitable for problems with many columns.

4. **No presolve for MIP** - Presolve only works for LP.

5. **Presolve disabled by default** - Adds overhead on random LPs, may help structured problems.

---

## Optimization History

### Commit 5a41745: Sparse U Extraction
- Changed U entry extraction to use row list
- Complexity: O(nnz) instead of O(m) per column

### Commit cef048f: Sparse Eta Storage
- Changed eta vectors from dense to sparse
- Memory: O(nnz) instead of O(m) per update
- Apply time: O(nnz) instead of O(m)

### Commit d925029: Fix Devex Pricing
- Compute pivot row BEFORE LU update (old basis)
- Critical bug fix for correct weight updates

### Commit bd9176a: LU Refactorization Thresholds
- max_updates: 50 → 100
- growth_factor threshold: 100 → 1e6
- Result: 3× speedup (34 → 11 refactorizations)

### Commit 82b6f80: Presolve Optimization
- All presolve functions: O(m×n²) → O(m×n)
- Extract row once, not element-by-element
- Presolve time: 84% → <1% of total

### Commit b188ccc: Devex Reference Reset
- Reset weights to 1.0 every n iterations
- Prevents unbounded weight growth

---

## Future Improvements

### High Priority

1. **Dual Simplex for Main Solve**
   - Would likely reduce iteration count 2-3×
   - Already implemented for MIP re-optimization
   - Needs to be exposed as solver option

2. **Better Initial Basis**
   - Implement crash procedure
   - Avoid artificial variables when possible

### Medium Priority

3. **Perturbation for Degeneracy**
   - Small random perturbations to RHS
   - Alternative to Bland's rule

4. **Steepest Edge Exact**
   - Currently only Devex (approximate)
   - Exact SE more iterations but fewer pivots

### Lower Priority

5. **Scaling**
   - Currently implemented but not thoroughly tested
   - Geometric mean scaling helps numerical stability

6. **Presolve for MIP**
   - Probing, clique detection, etc.

---

## Parameter Reference

| Parameter | Default | Description |
|-----------|---------|-------------|
| `verbose` | 0 | Print iteration info |
| `presolve` | 0 | Enable presolve (disabled - adds overhead) |
| `max_iterations` | 100000 | Iteration limit |
| `time_limit` | 3600.0 | Time limit in seconds |
| `pricing_strategy` | 2 | 0=Dantzig, 1=Steepest Edge, 2=Devex |
| `scaling` | 1 | Enable geometric mean scaling |

### Internal Constants (lp.h)

```c
#define RALPH_ZERO_TOL    1e-12  // Numerical zero
#define RALPH_FEAS_TOL    1e-6   // Feasibility tolerance
#define RALPH_OPT_TOL     1e-8   // Optimality tolerance
#define RALPH_PIVOT_TOL   1e-10  // Minimum pivot value
#define RALPH_INFINITY    1e30   // Representation of infinity
```
