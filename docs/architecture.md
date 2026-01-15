# Ralph LP/MIP Solver Architecture

## Overview

Ralph is a complete LP (Linear Programming) and MIP (Mixed Integer Programming) solver implementing industrial-strength algorithms. This document describes the high-level architecture and how the components interact.

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Public API (ralph.h)                      │
│  ralph_create, ralph_add_var, ralph_add_constraint, ralph_solve  │
└─────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Model Layer (model.c)                       │
│         LPModel: variables, constraints, bounds, objective       │
└─────────────────────────────────────────────────────────────────┘
                                 │
                    ┌────────────┴────────────┐
                    ▼                         ▼
┌──────────────────────────┐    ┌──────────────────────────┐
│   LP Solver (simplex.c)   │    │   MIP Solver (mip.c)     │
│   Revised Simplex Method  │◄───│   Branch and Bound       │
└──────────────────────────┘    └──────────────────────────┘
            │                              │
            ▼                              ▼
┌──────────────────────────┐    ┌──────────────────────────┐
│  LU Factorization (lu.c)  │    │  Cutting Planes (cuts.c) │
│  Basis matrix operations  │    │  Gomory, MIR cuts        │
└──────────────────────────┘    └──────────────────────────┘
            │
            ▼
┌──────────────────────────┐
│  Sparse Matrix (sparse.c) │
│  CSC format operations    │
└──────────────────────────┘
```

## Core Components

### 1. Sparse Matrix Layer (`sparse.c`, `sparse.h`)

**Purpose:** Efficient storage and operations for sparse matrices.

**Data Structure:**
```c
typedef struct {
    int nrows, ncols, nnz;
    int *colptr;    // Column pointers (size: ncols + 1)
    int *rowidx;    // Row indices (size: nnz)
    double *values; // Non-zero values (size: nnz)
} SparseMatrix;
```

**Key Operations:**
- Matrix-vector multiplication: O(nnz)
- Column extraction: O(column_nnz)
- Submatrix extraction

### 2. Model Layer (`model.c`, `lp.h`)

**Purpose:** Represent optimization problems in standard form.

**Data Structure:**
```c
typedef struct {
    int num_vars, num_cons;
    SparseMatrix *A;       // Constraint matrix
    double *c;             // Objective coefficients
    double *b;             // RHS values
    double *lb, *ub;       // Variable bounds
    char *sense;           // Constraint sense (L, G, E)
    char *var_type;        // Variable type (C, I, B)
    int obj_sense;         // 1 = minimize, -1 = maximize
} LPModel;
```

### 3. Simplex Tableau (`simplex.c`)

**Purpose:** Maintain the simplex tableau in revised form.

**Data Structure:**
```c
typedef struct {
    LPModel *model;
    int m, n;                  // Dimensions (constraints, total vars)
    SparseMatrix *A_ext;       // Extended matrix with slacks
    double *c_ext;             // Extended costs
    double *lb_ext, *ub_ext;   // Extended bounds

    int *basis;                // Basic variable indices
    int *basis_pos;            // Position of each var in basis (-1 if nonbasic)
    VarStatus *var_status;     // BASIC, NONBASIC_LOWER, NONBASIC_UPPER

    double *x;                 // Solution vector
    double *y;                 // Dual values
    double *rc;                // Reduced costs

    LUFactorization *lu;       // Basis factorization
} SimplexTableau;
```

### 4. LU Factorization (`lu.c`)

**Purpose:** Efficiently solve systems with the basis matrix.

**Key Insight:** Instead of refactorizing B after each pivot, we maintain:
- Initial factorization: PA = LU
- Eta matrices E₁, E₂, ... for subsequent updates
- B_current = B_initial × E₁ × E₂ × ... × Eₖ

**Data Structure:**
```c
typedef struct {
    int m;
    int *L_colptr, *L_rowidx;  // L matrix in CSC
    double *L_values;
    int *U_colptr, *U_rowidx;  // U matrix in CSC
    double *U_values;
    int *perm, *perm_inv;      // Row permutation

    // Eta file for updates
    int num_eta;
    int *eta_col;              // Which column each eta modifies
    double **eta_vectors;      // The eta vectors
} LUFactorization;
```

### 5. MIP Solver (`mip.c`, `branch_bound.c`)

**Purpose:** Solve mixed-integer problems via branch and bound.

**Components:**
- **Node Queue:** Priority queue of B&B nodes
- **Branching:** Select fractional variable, create child nodes
- **Bounding:** Solve LP relaxation at each node
- **Pruning:** Discard nodes that can't improve incumbent

## Algorithm Details

### Revised Simplex Method

The simplex method finds optimal solutions to LP problems by moving from vertex to vertex of the feasible region.

**Main Loop:**
```
1. Compute reduced costs: rc = c - y'A where y = B⁻ᵀcB
2. Pricing: Select entering variable j with rc[j] < 0 (for min)
3. Ratio test: Compute direction d = B⁻¹Aⱼ, find leaving variable
4. Pivot: Update basis, solution, and LU factorization
5. Repeat until optimal (no negative rc) or unbounded
```

**Pricing Strategies:**
- Dantzig: Most negative reduced cost
- Steepest Edge: Best improvement per unit movement

**Ratio Test:**
- Standard: θ = min{xB[i]/d[i] : d[i] > 0}
- Harris: Two-pass with tolerance for numerical stability

### Branch and Bound

For MIP problems with integer constraints:

```
1. Solve LP relaxation (root node)
2. If solution is integer-feasible → done
3. Select fractional variable xⱼ = f
4. Branch: Create nodes with xⱼ ≤ ⌊f⌋ and xⱼ ≥ ⌈f⌉
5. Process nodes in priority order
6. Prune nodes with bound worse than incumbent
7. Update incumbent when integer solution found
```

**Node Selection:**
- Best-first: Prioritize best LP bound
- Depth-first: Dive deep to find incumbents fast
- Hybrid: Mix of both strategies

### Cutting Planes

Strengthen LP relaxation to get tighter bounds:

**Gomory Mixed-Integer Cuts:**
For a basic variable xᵢ with fractional value:
```
∑ⱼ aᵢⱼxⱼ = bᵢ  (tableau row)

Cut: ∑ⱼ (fⱼ if fⱼ ≤ f₀, else f₀(1-fⱼ)/(1-f₀)) xⱼ ≥ f₀

where fⱼ = fractional part of aᵢⱼ, f₀ = fractional part of bᵢ
```

## Data Flow Example

**Solving: min -x - y, s.t. x + y ≤ 4, 2x + y ≤ 6, x,y ≥ 0**

1. **Model Creation:**
   ```
   Variables: x (idx 0), y (idx 1)
   Constraint matrix: [[1, 1], [2, 1]]
   RHS: [4, 6]
   Objective: [-1, -1]
   ```

2. **Tableau Creation:**
   ```
   Add slack s₁, s₂ for ≤ constraints
   Extended matrix: [[1, 1, 1, 0], [2, 1, 0, 1]]
   Extended costs: [-1, -1, 0, 0]
   Initial basis: [s₁, s₂] = [2, 3]
   ```

3. **Initial Solution:**
   ```
   x = 0, y = 0, s₁ = 4, s₂ = 6
   Objective = 0
   ```

4. **Simplex Iterations:**
   ```
   Iter 1: x enters, s₂ leaves → x = 3, y = 0, s₁ = 1, s₂ = 0
   Iter 2: y enters, s₁ leaves → x = 2, y = 2, s₁ = 0, s₂ = 0
   Optimal: obj = -4
   ```

## Error Handling

- **Infeasibility:** Detected via Big-M method (artificial vars remain)
- **Unboundedness:** Ratio test finds no blocking variable
- **Numerical Issues:** Refactorization, tolerances, perturbation

## Performance Characteristics

| Operation | Complexity |
|-----------|------------|
| Matrix-vector multiply | O(nnz) |
| LU factorization | O(m³) worst case, often O(m × nnz) |
| Simplex iteration | O(m²) typical |
| MIP node processing | O(LP solve) |
| Branch and bound | Exponential worst case |
