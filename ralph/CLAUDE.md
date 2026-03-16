# Claude Code Instructions for Ralph Solver

## Overview

**Ralph** (**R**obust **A**I **L**inear **P**rogramming **H**elper) is a C library implementing LP, MIP, LAP, and Network Flow solvers. It has zero external dependencies and is designed to be embedded in other projects.

## Quick Start

```bash
make          # Build libralph.a
make test     # Run tests (378/378 should pass)
make test-lap # Run LAP tests (358/358 should pass)
make test-netflow # Run Network Flow tests (153/153 should pass)
```

## Key Files

| File | Purpose |
|------|---------|
| `include/ralph_lp.h` | LP public API - start here |
| `include/ralph_mip.h` | MIP public API |
| `include/ralph_core.h` | Internal core API (non-public) |
| `include/lap.h` | LAP solver API |
| `include/netflow.h` | Network Flow solver API |
| `include/detect.h` | Problem structure detection |
| `src/simplex.c` | Primal simplex algorithm |
| `src/lu.c` | LU factorization (critical) |
| `src/lap.c` | JVC algorithm for LAP |
| `src/netflow.c` | Network simplex algorithm |
| `src/detect.c` | LAP/network detection |
| `src/branch_bound.c` | MIP solver |
| `tests/test_main.c` | LP/MIP test suite |
| `tests/test_lap.c` | LAP test suite |
| `tests/test_netflow.c` | Network Flow test suite |

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
    ↓              ↓                ↓
┌─────────────┐ ┌─────────────┐ ┌─────────────┐
│ LAP Solver  │ │  Network    │ │   Simplex   │
│  (lap.c)    │ │  Simplex    │ │ (simplex.c) │
│  O(n³) JVC  │ │ (netflow.c) │ │     ↕       │
└─────────────┘ └─────────────┘ │  LU (lu.c)  │
                                └─────────────┘
                                      ↓
                           MIP (mip.c) → Branch & Bound
                           (uses LAP for assignment MIPs)
```

## Critical Invariants

1. **CSC format**: Sparse matrices are column-major
2. **LU eta-file**: Updates must modify all components
3. **Constraint normalization**: RHS must be non-negative
4. **Two-phase simplex**: Artificial variables use Phase 1 (cost=1.0), no Big-M
5. **LAP costs**: Row-major n×n matrix, use RALPH_LAP_INFINITY for forbidden

## LAP Solver Features

The LAP solver implements the Jonker-Volgenant-Castanon (JVC) algorithm:

| Feature | API | Notes |
|---------|-----|-------|
| Dense LAP | `ralph_lap_solve()` | O(n³), SIMD optimized |
| Sparse LAP | `ralph_lap_solve_sparse()` | CSR format, auto-fallback to dense |
| Rectangular | `ralph_lap_solve_rect()` | m×n problems |
| Warm start | `ralph_lap_solve_warm()` | Reuse dual variables |
| Callbacks | `ralph_lap_solve_callback()` | O(n) memory for huge problems |
| k-Best | `ralph_lap_solve_k_best()` | Murty's algorithm |
| Bottleneck | `ralph_lap_solve_ex()` | Minimax/maximin assignment |
| Priority | `ralph_lap_solve_ex()` | Row/column priorities for unbalanced LAP |
| Cardinality | `ralph_lap_solve_ex()` | Min/max assignment bounds |
| Qualification | `ralph_lap_solve_ex()` | Allow-list subsets for columns |
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
| Rectangular | ✓ | ✓* | ✓ | - |

*Sparse, callback, and rectangular k-best convert to dense/square internally.

### Priority Constraints

For unbalanced rectangular LAP (m ≠ n), priorities determine which agents/jobs get assigned when there are more rows than columns (or vice versa). Priority values range from 1-10 where 10 = highest priority.

```c
/* 5 workers competing for 3 jobs - high priority workers get assigned */
double cost[15] = { /* 5×3 cost matrix */ };
int row_prio[5] = {10, 2, 8, 1, 9};  /* Workers 0,4,2 have priority */

RalphLapProblem prob = {
    .n = 5, .m = 3,
    .cost_type = RALPH_LAP_COST_DENSE,
    .dense_cost = cost,
    .objective = RALPH_LAP_MINIMIZE
};

RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
opts.num_row_priorities = 5;
opts.row_priorities = row_prio;

int row_sol[5];
RalphLapResult res = {.row_sol = row_sol};
ralph_lap_solve_ex(&prob, &opts, &res, NULL);
/* Workers 0,4,2 assigned; workers 1,3 get -1 (unassigned) */
```

**Implementation**: Costs are transformed by adding penalty `M*(10-priority)` to each row/column. For dummy assignments (unassigned slots), high-priority agents have expensive dummy costs, ensuring they prefer real assignments. Maintains O(n³) complexity.

**Options fields**:
- `num_row_priorities` / `row_priorities`: Priority array for rows (0 = disabled)
- `num_col_priorities` / `col_priorities`: Priority array for columns (0 = disabled)

Combines with other options (forbidden, cardinality, qualifications, maximization).

### Cardinality Bounds

Control how many pairs get matched:

```c
/* 6 workers, 4 jobs - but only assign 2 pairs */
opts.max_assignments = 2;  /* At most 2 assigned */
opts.min_assignments = 2;  /* At least 2 required (returns INFEASIBLE if impossible) */
```

When `max_assignments < min(m,n)`, priorities determine which rows/columns get assigned. Useful for budget constraints or partial fulfillment scenarios.

### Qualification Subsets

Allow-list for columns - specify which rows can serve each column. More ergonomic than forbidden when few rows qualify (e.g., 3 of 100 drivers are hazmat-certified).

```c
/* Job 0: only workers 0,1 qualified; Job 2: only workers 2,3 qualified */
int qual_col_idx[] = {0, 2};
int qual_row_ptr[] = {0, 2, 4};
int qual_rows[] = {0, 1, 2, 3};

opts.num_qual_cols = 2;
opts.qual_col_idx = qual_col_idx;
opts.qual_row_ptr = qual_row_ptr;
opts.qual_rows = qual_rows;
```

Uses CSR-like sparse format. Columns not listed have no restrictions. Combines with priorities, cardinality, and forbidden constraints.

**Note**: Cardinality and qualification constraints require dense cost representation (not supported for sparse/callback).

### Problem Detection

Ralph auto-detects LAP and network flow structure in LP/MIP models:

```c
// Enable detection for a specific model
ralph_set_int_param(model, "detect_special", 1);

// Global toggles (both enabled by default)
ralph_set_detect_lap(1);       // LAP detection
ralph_set_detect_network(1);   // Network flow detection
```

When enabled:
- Assignment problems are solved with JVC instead of simplex (86-633× faster)
- Network flow problems are solved with network simplex (5-10× faster)

## Network Flow Solver Features

The network flow solver implements the network simplex algorithm for Minimum Cost Network Flow (MCNF) problems:

| Feature | API | Notes |
|---------|-----|-------|
| Standard MCNF | `ralph_netflow_solve()` | Candidate list pricing |
| Unified API | `ralph_netflow_solve_ex()` | Supports all algorithm variants |
| Flow decomposition | `ralph_netflow_decompose()` | Decompose flow into paths |
| Bottleneck | `algorithm = BOTTLENECK` | Minimize max arc cost used |
| Warm start | `options.warm_start = 1` | Reuse basis from previous solve |
| Cost scaling | `options.cost_scaling = 1` | Epsilon scaling for degeneracy |
| Pricing rules | `options.pricing` | CANDIDATE, FIRST, BEST |
| Workspace reuse | `ralph_netflow_workspace_*()` | Amortize allocations |

### Basic Usage

```c
int tail[] = {0, 1};
int head[] = {1, 2};
double cost[] = {1.0, 2.0};
double supply[] = {5.0, 0.0, -5.0};  /* Negative = demand */

RalphNetflowProblem prob = {
    .num_nodes = 3, .num_arcs = 2,
    .tail = tail, .head = head, .cost = cost,
    .capacity = NULL,  /* NULL = infinite */
    .lower = NULL,     /* NULL = zero */
    .supply = supply,
    .objective = RALPH_NETFLOW_MINIMIZE
};

RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
double flow[2];
RalphNetflowResult result = {.flow = flow};

ralph_netflow_solve(&prob, &opts, &result, NULL);
/* result.objective = 15.0, flow = {5.0, 5.0} */
```

### Warm Start for Re-optimization

```c
RalphNetflowWorkspace *ws = ralph_netflow_workspace_create(100, 500);
RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
opts.save_warm_start = 1;  /* Save basis after solve */

/* First solve */
ralph_netflow_solve(&prob1, &opts, &result, ws);

/* Second solve with changed costs - uses warm start automatically */
opts.warm_start = 1;
ralph_netflow_solve(&prob2, &opts, &result, ws);  /* Much faster */

ralph_netflow_workspace_free(ws);
```

### Unified API (Extended Features)

The unified API supports multiple algorithm variants via `ralph_netflow_solve_ex()`:

```c
RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;

/* Flow decomposition: get k best paths */
opts.algorithm = RALPH_NETFLOW_ALG_K_BEST;
opts.k = 5;

RalphNetflowPath paths[5];
double objectives[5];
RalphNetflowResult result = {
    .flow = flow,
    .paths = paths,
    .objectives = objectives
};
ralph_netflow_solve_ex(&prob, &opts, &result, NULL);
/* result.num_found = number of paths, paths sorted by unit_cost */

/* Bottleneck: minimize maximum arc cost used */
opts.algorithm = RALPH_NETFLOW_ALG_BOTTLENECK;
ralph_netflow_solve_ex(&prob, &opts, &result, NULL);
/* result.objective = bottleneck cost (max arc cost used) */

/* Cost scaling for degenerate problems */
opts.algorithm = RALPH_NETFLOW_ALG_STANDARD;
opts.cost_scaling = 1;
opts.epsilon_factor = 4.0;  /* Reduction factor per phase */
ralph_netflow_solve_ex(&prob, &opts, &result, NULL);
```

### Problem Detection

Network flow structure is auto-detected in LP models (enabled by default):

```c
// Per-model detection (required)
ralph_set_int_param(model, "detect_special", 1);

// Global toggle (enabled by default)
ralph_set_detect_network(1);
```

Detects: SHORTEST_PATH, ASSIGNMENT, TRANSPORTATION, and GENERAL network problems. Assignment problems are delegated to the LAP solver for optimal performance.

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

### Working with Network Flow solver
1. Basic: `ralph_netflow_solve()` with Problem/Options/Result structs
2. Warm start: Set `opts.save_warm_start = 1`, then `opts.warm_start = 1`
3. Workspace reuse: Create once, solve multiple problems
4. Integration: Enable `detect_special` param for auto network detection

### Adding Network Flow features
1. Edit `src/netflow.c` for algorithm changes
2. Follow unified API pattern (Options struct for orthogonal features)
3. Add tests to `tests/test_netflow.c`
4. Run `make test-netflow` to verify

## Testing

```bash
# All LP/MIP tests (73 tests)
make test

# LAP tests only (358 tests)
make test-lap

# Network Flow tests (153 tests)
make test-netflow

# LP only (faster)
./test_ralph --skip-mip

# Add new test
# Edit tests/test_main.c for LP/MIP
# Edit tests/test_lap.c for LAP
# Edit tests/test_netflow.c for Network Flow
```

## Benchmarks

```bash
make bench-lap        # LAP benchmarks (size scaling, sparse vs dense)
make bench-lap mip    # LAP-based MIP benchmark
make bench-netflow    # Network Flow benchmarks (size, warm start, bottleneck)
```

## Code Style

- 4-space indentation
- `snake_case` functions and variables
- `SCREAMING_CASE` constants
- Comments for non-obvious logic

## Numerical Tolerances

```c
/* General */
#define TOLERANCE 1e-9      // General numerical tolerance
#define PIVOT_TOL 1e-10     // Minimum pivot value

/* Network Flow */
#define RALPH_NETFLOW_TOLERANCE 1e-9   // Flow/cost tolerance
#define RALPH_NETFLOW_BIG_M 1e12       // Artificial arc cost
#define RALPH_NETFLOW_INFINITY 1e15    // Infinite capacity
```

## Memory Management

- Models freed with `ralph_free()`
- Internal allocations use standard malloc/free
- Use `SAFE_FREE(p)` macro to NULL pointer after freeing
- Pre-allocate workspaces in hot paths (avoid malloc in loops)

## Code Quality Documentation

See `docs/CODE_QUALITY.md` for:
- Known issues and their status
- Security considerations
- Performance optimization opportunities
- Arena allocator recommendation

## Critical Code Sections

### LU Factorization (`src/lu.c`)
Most sensitive - bugs cause wrong solutions:
- `lu_factorize()` - Initial factorization
- `lu_solve()` - Solve Bx = b
- `lu_update()` - Basis change update

### Simplex Tableau (`src/simplex.c`)
- `tableau_create()` - Extended problem setup with slacks/artificials
- `simplex_pivot()` - Basis change operation
- `pricing_*()` - Variable selection strategies

### Sparse Matrix (`src/sparse.c`)
- `triplets_to_csc()` - Validate indices before conversion
- Column operations must check bounds

## Coding Practices

### Safe Memory Patterns
```c
/* Always use SAFE_FREE to prevent double-free */
SAFE_FREE(p);  /* Sets p = NULL after free */

/* Check realloc return before assignment */
double *new_p = realloc(p, size);
if (!new_p) return -1;
p = new_p;

/* Validate array indices before access */
if (idx < 0 || idx >= count) return ERROR;
```

### Integer Overflow Prevention
```c
/* Use size_t for allocation sizes */
size_t pool_size = (size_t)a * (size_t)b;
if (pool_size > INT_MAX) pool_size = INT_MAX;

/* Or use calloc which checks internally */
void *p = calloc(count, element_size);
```

### Bounds Validation
```c
/* Always validate external indices */
if (row < 0 || row >= nrows || col < 0 || col >= ncols) {
    return NULL;  /* or appropriate error */
}
```

### Constants Over Magic Numbers
```c
/* Good - use defined constants */
if (infeas > RALPH_FEAS_TOL) { ... }

/* Bad - magic numbers */
if (infeas > 1e-6) { ... }
```
