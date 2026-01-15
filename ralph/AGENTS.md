# Agents Guide for Ralph LP/MIP Solver

## Overview

**Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper) is a standalone Linear Programming (LP) and Mixed Integer Programming (MIP) solver written in C with zero external dependencies. It implements production-grade algorithms including the Revised Simplex method, Dual Simplex, Branch and Bound, and cutting plane generation.

## Directory Structure

```
ralph/
├── include/
│   ├── ralph.h       # Public API - main entry point
│   ├── sparse.h      # Sparse matrix structures (CSC format)
│   ├── lp.h          # Internal LP structures
│   ├── mip.h         # Internal MIP structures
│   └── presolve.h    # Presolve data structures
├── src/
│   ├── ralph.c       # Public API implementation
│   ├── model.c       # LP model building
│   ├── sparse.c      # Sparse matrix operations
│   ├── simplex.c     # Primal Revised Simplex
│   ├── dual_simplex.c # Dual Simplex
│   ├── lu.c          # LU factorization (CRITICAL)
│   ├── presolve.c    # Preprocessing
│   ├── branch_bound.c # MIP branch and bound
│   ├── cuts.c        # Cutting planes (Gomory)
│   ├── mip.c         # MIP orchestration
│   └── mps_reader.c  # MPS file parser
├── tests/
│   └── test_main.c   # Test suite (43 tests)
├── Makefile
├── AGENTS.md         # This file
└── CLAUDE.md
```

## Build Commands

```bash
make              # Build libralph.a and test_ralph
make test         # Run test suite
make clean        # Clean build artifacts
./test_ralph      # Run all tests (43/43 should pass)
./test_ralph --skip-mip  # LP tests only (faster)
```

## Key Concepts

### Sparse Matrix Format (CSC)
All matrices use Compressed Sparse Column format:
- `colptr[j]` to `colptr[j+1]` gives non-zeros in column j
- `rowidx[p]` gives row index of element p
- `values[p]` gives value of element p

### Revised Simplex Method
Core LP algorithm in `simplex.c`:
- Maintains LU factorization of basis matrix B
- Eta-file updates for efficient basis changes
- Steepest edge pricing for variable selection
- Harris ratio test for numerical stability

### Big-M Method
Constraints handled by auxiliary variables:
- `<=`: Add slack (cost 0)
- `>=`: Add surplus (0) + artificial (1e8)
- `=`: Add artificial (1e8)

### MIP Solving
Branch and bound with:
- Best-first node selection
- Pseudo-cost branching
- Gomory mixed-integer cuts
- Rounding heuristics

## Critical Code Sections

### LU Factorization (`src/lu.c`)
Most sensitive code - bugs cause wrong solutions.

**Key functions:**
- `lu_factorize()` - Initial factorization with Markowitz ordering
- `lu_solve()` - Solve Bx = b
- `lu_update()` - Update after basis change
- `apply_eta_forward()` - Apply eta matrices

### Tableau Creation (`src/simplex.c:tableau_create`)
1. Normalizes constraints (makes RHS non-negative)
2. Adds slack/artificial variables
3. Initializes basis
4. Computes initial basic variable values

### Infeasibility Detection
After optimization, checks if artificial variables remain positive.
If objective > 1e6, problem is infeasible.

## Public API (`include/ralph.h`)

```c
// Model creation
RalphModel *ralph_create(void);
void ralph_free(RalphModel *model);

// Variables
int ralph_add_var(RalphModel *model, double lb, double ub,
                  double obj_coef, RalphVarType type);

// Constraints
int ralph_add_constraint(RalphModel *model, int nnz,
                        const int *indices, const double *values,
                        RalphConstraintType type, double rhs);

// Solving
int ralph_optimize(RalphModel *model);
RalphStatus ralph_get_status(RalphModel *model);
double ralph_get_objval(RalphModel *model);
void ralph_get_solution(RalphModel *model, double *x);
```

## Common Pitfalls

1. **Forgetting constraint normalization** - Negative RHS breaks initial basis
2. **Eta update bugs** - Must modify all components, not just pivot column
3. **Column-major confusion** - Sparse matrices are column-major
4. **MIP gap tolerance** - Default is 15%, tighten for better solutions

## Testing

```bash
# Full test suite
make test

# Quick LP-only tests
./test_ralph --skip-mip

# Add new test in tests/test_main.c
# Use ASSERT() and ASSERT_NEAR() macros
```

## Performance

- LP: O(n³) worst case, typically much faster
- Refactorization every 50 eta updates
- MIP: Exponential worst case, use node limits
- Sparse operations: O(nnz), not O(n×m)

## Known Limitations

1. **MILP rounding heuristic**: May violate indicator constraints (documented)
2. **No interior point**: Simplex only
3. **Single-threaded**: No parallel branch-and-bound
