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
- **Cut Callbacks:** User-provided cut generation at each node
- **Branch Callbacks:** User-provided variable selection override

**MIP Infrastructure (Feb 2026):**

The solver supports domain-specific customization via callbacks and priorities:

```c
/* Branching control */
void ralph_set_branch_priorities(RalphModel *m, const int *priorities);
void ralph_set_branch_directions(RalphModel *m, const int *directions);

/* Warm start for re-optimization */
RalphBasis* ralph_save_basis(const RalphModel *m);
int ralph_load_basis(RalphModel *m, const RalphBasis *basis);
void ralph_free_basis(RalphBasis *basis);

/* Cut callback - invoked at each B&B node */
typedef struct {
    int (*generate_cuts)(void *user_data, const double *x_relaxation,
                         int num_vars, RalphCut *cuts, int max_cuts);
    void *user_data;
} RalphCutCallback;
void ralph_set_cut_callback(RalphModel *m, const RalphCutCallback *cb);

/* Branch callback - custom variable selection */
typedef struct {
    int (*select_branch_var)(void *user_data, const double *x_relaxation,
                              int num_vars, const int *is_integer,
                              const double *lb, const double *ub);
    void *user_data;
} RalphBranchCallback;
void ralph_set_branch_callback(RalphModel *m, const RalphBranchCallback *cb);

/* Lazy constraints - add cuts and re-solve */
int ralph_add_lazy_constraint(RalphModel *m, const RalphCut *cut);
int ralph_add_lazy_constraints(RalphModel *m, const RalphCut *cuts, int count);
```

**Use cases:**
- **FuelWise:** Reach cuts for reachability constraints, Benders decomposition
- **HoSE:** Driving capacity cuts (11h limit), mandatory break cuts (8h)
- **General:** Custom branching priorities, domain-specific cut generation

### 6. Benders Decomposition (`benders.c`)

**Purpose:** Solve problems with complicating variables via master/subproblem decomposition.

**Algorithm:**
1. Partition model: master vars (integer/binary) vs subproblem vars (continuous)
2. Detect linking constraints (constraints involving both)
3. Solve master MIP with theta variable for recourse cost
4. Fix master solution, solve subproblem LP
5. Generate cuts from subproblem duals (optimality) or Farkas rays (feasibility)
6. Repeat until convergence

**Data Structures:**
```c
typedef struct {
    int *master_var_indices;    /* Which vars go to master */
    int num_master_vars;
    int theta_var;              /* Recourse cost variable (-1 = auto) */
    int num_scenarios;          /* 1 for deterministic */
    double *scenario_probs;     /* Weights for stochastic */
    double gap_tolerance;       /* Convergence criterion */
    int max_iterations;
    int cuts_at_lp_nodes;       /* 1 = modern B&B&C (not yet implemented) */
    int warm_start_subproblems; /* Reuse subproblem basis */
} RalphBendersConfig;

typedef struct {
    int status;                 /* OPTIMAL, INFEASIBLE, etc. */
    double objective;
    int iterations;
    int optimality_cuts;        /* Cuts generated */
    int feasibility_cuts;
} RalphBendersResult;
```

**Internal Context:**
```c
typedef struct {
    LPModel *master_model;      /* Master MIP */
    LPModel *sub_model;         /* Subproblem LP */
    int *master_to_orig;        /* Variable mapping */
    int *sub_to_orig;
    LinkingConstraint *linking; /* Constraints coupling master↔sub */
    int num_linking;
    double *master_solution;    /* Current master solution */
    BendersCut *cuts;           /* Accumulated cuts */
    int num_cuts;
} BendersContext;
```

**Cut Generation:**
- **Optimality cut:** `θ >= π'(h - Tx)` where π = subproblem duals
- **Feasibility cut:** `0 >= y'(h - Tx)` where y = Farkas ray

**Key Implementation Details:**
- Model finalization: After adding cuts, must re-finalize master (rebuilds sparse matrix)
- Numerical stability: Avoid RALPH_INFINITY bounds on theta; use domain-appropriate limits
- Linking detection: Scans constraint matrix for mixed master/sub variable references

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

## Current Performance (vs GLPK 5.0)

### LP Benchmarks (Sparse LPs, 10% density)

| Problem Size | Ralph Time | GLPK Time | Slowdown |
|--------------|------------|-----------|----------|
| 50×100       | 0.0002s    | 0.0001s   | 2.0×     |
| 100×200      | 0.0015s    | 0.0009s   | 1.7×     |
| 200×500      | 0.021s     | 0.009s    | 2.3×     |
| 500×1000     | 6.88s      | 1.50s     | **4.6×** |
| 1000×2000    | 105.0s     | 36.2s     | **2.9×** |

### LP Benchmarks (Medium density, 30%)

| Problem Size | Ralph Time | GLPK Time | Slowdown |
|--------------|------------|-----------|----------|
| 200×500      | 0.58s      | 0.13s     | 4.5×     |
| 500×1000     | 73.7s      | 14.5s     | **5.1×** |

### Per-Iteration Time Breakdown (200×400 problem)

| Operation | Time | Percentage |
|-----------|------|------------|
| **Ratio test (FTRAN)** | 0.159s | **63.4%** |
| **B^-1 update** | 0.089s | **35.3%** |
| Pricing | 0.002s | 1.0% |
| Pivot | 0.001s | 0.4% |

**Key Finding:** LU operations (FTRAN + Update) account for 98.7% of solver time.
The main bottleneck is the sparse LU solve in the ratio test.

### MIP Benchmarks (Classic Problems, Quick Mode)

| Problem Type      | Size       | Ralph       | GLPK    | Notes                    |
|-------------------|------------|-------------|---------|--------------------------|
| SetCovering       | 20×10      | 0.0001s ✓   | 0.0001s | Matches objective        |
| SetCovering       | 40×20      | 0.0002s ✓   | 0.0002s | Matches objective        |
| SetPartitioning   | 30×10      | 36.9s ⚠     | 0.0002s | ~185,000× slower         |
| SetPartitioning   | 60×20      | 63.4s ⚠     | 0.003s  | Hit time limit           |
| LinearAssignment  | 100×20     | 0.0001s ✓   | 0.0003s | Matches objective        |
| LinearAssignment  | 400×40     | 0.0005s ✓   | 0.002s  | Matches objective        |
| NetworkFlow (LP)  | 77×20      | 0.0001s ✓   | 0.0000s | Matches objective        |
| NetworkFlow (LP)  | 295×40     | 0.0002s ⚠   | 0.0001s | **Objective mismatch**   |
| FacilityLocation  | 55×60      | 0.0001s ✓   | 0.0003s | Matches objective        |
| FacilityLocation  | 210×220    | 160.7s ⚠    | 0.002s  | ~70,000× slower          |

**Key Findings:**
- LP relaxations solved correctly and quickly
- MIP enumeration is extremely slow on hard combinatorial problems
- One potential correctness issue with NetworkFlow (25.42 vs 125.42)

## Known Issues and TODO

### High Priority

1. **LP Per-Iteration Performance (2-5× slowdown vs GLPK)**
   - Root cause: FTRAN (63%) and LU updates (35%) dominate iteration time
   - Hyper-sparse FTRAN exists (`lu_ftran_hyper_sparse`) but disabled due to instability
   - TODO: Fix hyper-sparse FTRAN numerical issues and enable in ratio test
   - TODO: Reduce FT spike accumulation overhead
   - TODO: Consider batched LU updates

2. **MIP Branch-and-Bound Performance**
   - SetPartitioning 185,000× slower than GLPK
   - TODO: Implement proper node presolve (bound tightening, probing)
   - TODO: Add pseudocost branching or reliability branching
   - TODO: Implement diving heuristics for faster incumbent finding

3. **Dual Simplex Stability**
   - Currently falls back to primal on many problems due to RC drift
   - Root cause: Calling lu_solve per column compounds error (n sources per pivot)
   - TODO: Compute pivot row via single BTRAN, update RCs with sparse dot products
   - TODO: Periodic RC recomputation every ~20 iterations
   - TODO: Iterative refinement for reduced costs

### Medium Priority

4. **LP-Aware LU Updates**
   - Schur complement helps initial factorization (4× speedup)
   - TODO: Extend block structure awareness to LU updates
   - TODO: Block-aware solve routines

5. **Presolve Improvements**
   - TODO: Dominated rows/columns elimination
   - TODO: Probing on integer variables
   - TODO: Clique detection from set-packing constraints

### Low Priority

6. **Cut Generation**
   - GMI cut infrastructure implemented but disabled (produces incorrect cuts on some problems)
   - TODO: Debug GMI cut formula for >= constraints with negative coefficients
   - TODO: Lift-and-project cuts
   - TODO: Clique cuts from conflict graph

7. **Parallel Processing**
   - TODO: Parallel pricing in simplex
   - TODO: Parallel node processing in B&B

## Recent Optimizations

### LU Factorization (January 2025)

1. **U Diagonal Cache** (`lu.c`)
   - Added `U_diag[]` array for O(1) diagonal access
   - Eliminates O(m) search per column in backward substitution

2. **LP-Aware Factorization with Schur Complement** (`lu_sparse.c`)
   - Exploits LP basis structure: identity columns (slacks) vs structural columns
   - For k structural columns out of m: O(k³) instead of O(m³)
   - Handles cross-terms (structural entries in identity rows) via Schur complement
   - Block structure: L = [L11, 0; L21, I], U = [U11, 0; 0, D]
   - **Result:** 4× factorization speedup on m=500, k=200

3. **Spike Pool Allocation** (`lu.c`)
   - Pre-allocated storage for Forrest-Tomlin update spikes
   - Eliminates malloc in LU update hot path

4. **Reach-Based Sparse Triangular Solves** (`lu.c`)
   - Compute reach of sparse RHS before solving
   - Only touch non-zero elements in solution
