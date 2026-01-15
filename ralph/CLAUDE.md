# Claude Code Instructions for Ralph Solver

## Overview

**Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper) is a C library implementing LP and MIP solvers. It has zero external dependencies and is designed to be embedded in other projects.

## Quick Start

```bash
make          # Build libralph.a
make test     # Run tests (43/43 should pass)
```

## Key Files

| File | Purpose |
|------|---------|
| `include/ralph.h` | Public API - start here |
| `src/simplex.c` | Primal simplex algorithm |
| `src/lu.c` | LU factorization (critical) |
| `src/branch_bound.c` | MIP solver |
| `tests/test_main.c` | Test suite |

## Architecture

```
User API (ralph.c)
    ↓
Model Building (model.c)
    ↓
Simplex (simplex.c) ←→ LU (lu.c)
    ↓
MIP (mip.c) → Branch & Bound (branch_bound.c)
```

## Critical Invariants

1. **CSC format**: Sparse matrices are column-major
2. **LU eta-file**: Updates must modify all components
3. **Constraint normalization**: RHS must be non-negative
4. **Big-M method**: Artificial variable cost is 1e8

## Common Tasks

### Adding a new pricing strategy
1. Edit `simplex.c`, look for `select_entering_variable()`
2. Add new selection logic
3. Test with existing test cases

### Modifying cut generation
1. Edit `cuts.c`
2. Cuts are added in `generate_gomory_cuts()`
3. Test with MIP test cases

### Debugging numerical issues
1. Check `lu.c` first - most bugs originate here
2. Verify constraint normalization in `tableau_create()`
3. Create debug test in `tests/debug_*.c`

## Testing

```bash
# All tests
./test_ralph

# LP only (faster)
./test_ralph --skip-mip

# Add new test
# Edit tests/test_main.c, use ASSERT() macros
```

## Code Style

- 4-space indentation
- `snake_case` functions and variables
- `SCREAMING_CASE` constants
- Comments for non-obvious logic

## Numerical Tolerances

```c
#define TOLERANCE 1e-9      // General numerical tolerance
#define PIVOT_TOL 1e-10     // Minimum pivot value
#define BIG_M 1e8           // Artificial variable cost
```

## Memory Management

- Models freed with `ralph_free()`
- Internal allocations use standard malloc/free
- No memory pools or custom allocators
