# Claude Code Instructions for Ralph Solver

## Overview

**Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper) is a C library implementing LP, MIP, and LAP solvers. It has zero external dependencies and is designed to be embedded in other projects.

## Quick Start

```bash
make          # Build libralph.a
make test     # Run tests (73/73 should pass)
make test-lap # Run LAP tests (255/255 should pass)
```

## Key Files

| File | Purpose |
|------|---------|
| `include/ralph.h` | Public API - start here |
| `include/lap.h` | LAP solver API |
| `include/detect.h` | Problem structure detection |
| `src/simplex.c` | Primal simplex algorithm |
| `src/lu.c` | LU factorization (critical) |
| `src/lap.c` | JVC algorithm for LAP |
| `src/detect.c` | LAP/network detection |
| `src/branch_bound.c` | MIP solver |
| `tests/test_main.c` | LP/MIP test suite |
| `tests/test_lap.c` | LAP test suite |

## Architecture

```
User API (ralph.c)
    ↓
Model Building (model.c)
    ↓
┌─────────────────────────────────────────┐
│         Problem Detection (detect.c)    │
│    Detects LAP/network structure        │
└─────────────────────────────────────────┘
    ↓                    ↓
┌─────────────┐    ┌─────────────┐
│ LAP Solver  │    │   Simplex   │
│  (lap.c)    │    │ (simplex.c) │
│  O(n³) JVC  │    │     ↕       │
└─────────────┘    │  LU (lu.c)  │
                   └─────────────┘
                         ↓
              MIP (mip.c) → Branch & Bound
              (uses LAP for assignment MIPs)
```

## Critical Invariants

1. **CSC format**: Sparse matrices are column-major
2. **LU eta-file**: Updates must modify all components
3. **Constraint normalization**: RHS must be non-negative
4. **Big-M method**: Artificial variable cost is 1e8
5. **LAP costs**: Row-major n×n matrix, use RALPH_LAP_INFINITY for forbidden

## LAP Solver Features

The LAP solver implements the Jonker-Volgenant-Castanon (JVC) algorithm:

| Feature | API | Notes |
|---------|-----|-------|
| Dense LAP | `ralph_lap_solve()` | O(n³), SIMD optimized |
| Sparse LAP | `ralph_lap_solve_sparse()` | CSR format, auto-fallback to dense |
| Rectangular | `ralph_lap_solve_rect()` | m×n problems (m ≤ n) |
| Warm start | `ralph_lap_solve_warm()` | Reuse dual variables |
| Callbacks | `ralph_lap_solve_callback()` | O(n) memory for huge problems |
| k-Best | `ralph_lap_solve_k_best()` | Murty's algorithm |
| Bottleneck | `ralph_lap_solve_ex()` | Minimax/maximin assignment |
| ε-scaling | `ralph_lap_set_epsilon_scaling()` | For degenerate problems |

### Unified API (Recommended)

The unified API supports all feature combinations through a single function:

```c
RalphLapProblem prob = {
    .n = 100, .m = 100,
    .cost_type = RALPH_LAP_COST_DENSE,  /* or SPARSE, CALLBACK */
    .dense_cost = cost_matrix,
    .objective = RALPH_LAP_MINIMIZE
};

RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
opts.algorithm = RALPH_LAP_ALG_K_BEST;  /* or STANDARD, BOTTLENECK */
opts.k = 5;
opts.warm_start = 1;

int solutions[500];
double costs[5];
RalphLapResult res = {.row_sol = solutions, .costs = costs};

ralph_lap_solve_ex(&prob, &opts, &res, workspace);
```

**Feature matrix via unified API:**
| Cost Type | Standard | k-Best | Bottleneck | Warm Start |
|-----------|----------|--------|------------|------------|
| Dense | ✓ | ✓ | ✓ | ✓ |
| Sparse | ✓ | ✓* | ✓ | ✓ |
| Callback | ✓ | ✓* | - | - |
| Rectangular | ✓ | - | - | - |

*Sparse and callback k-best convert to dense internally.

### Problem Detection

Ralph can auto-detect LAP structure in LP/MIP models:

```c
// Enable detection for a specific model
ralph_set_int_param(model, "detect_special", 1);

// Global toggle (default: disabled)
ralph_set_detect_lap(1);
```

When enabled, assignment problems formulated as LPs/MIPs are solved with JVC instead of simplex (86-633× faster for LP relaxations).

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

### Working with LAP solver
1. Dense: `ralph_lap_solve()` for n×n cost matrix
2. Sparse: `ralph_lap_solve_sparse()` with CSR format
3. k-best: `ralph_lap_solve_k_best()` for alternative solutions
4. Integration: Enable `detect_special` param for auto LAP detection

### Adding LAP features
1. Edit `src/lap.c` for core algorithm changes
2. Edit `src/detect.c` for detection/integration changes
3. Add tests to `tests/test_lap.c`
4. Run `make test-lap` to verify

## Testing

```bash
# All LP/MIP tests (73 tests)
make test

# LAP tests only (255 tests)
make test-lap

# LP only (faster)
./test_ralph --skip-mip

# Add new test
# Edit tests/test_main.c for LP/MIP, tests/test_lap.c for LAP
```

## Benchmarks

```bash
make bench-lap        # LAP benchmarks (size scaling, sparse vs dense)
make bench-lap mip    # LAP-based MIP benchmark
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
