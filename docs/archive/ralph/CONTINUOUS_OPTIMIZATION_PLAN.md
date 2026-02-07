# Continuous Optimization Submodule Plan

**Date:** 2026-02-05
**Status:** Complete (Phases 1-3 Implemented)
**Motivation:** Lagrangian relaxation is a generic technique useful beyond SCP. Extract into reusable module.

## Implementation Status

| Phase | Component | Status | Files |
|-------|-----------|--------|-------|
| 1.1 | Subgradient interface | **Complete** | `include/optim_subgradient.h` |
| 1.2 | Subgradient implementation | **Complete** | `src/optim/subgradient.c` |
| 1.3 | Subgradient tests | **Complete** | `tests/test_optim.c` (11 tests) |
| 2.1 | Lagrangian interface | **Complete** | `include/optim_lagrangian.h` |
| 2.2 | Lagrangian implementation | **Complete** | `src/optim/lagrangian.c` |
| 2.3 | Lagrangian tests | **Complete** | `tests/test_optim.c` (9 tests) |
| 3.1 | SCP callbacks | **Complete** | `src/optim/lagrangian_scp.c` |
| 3.2 | Migration | **Complete** | Removed ~400 lines from `branch_bound.c` |

## Executive Summary

The current Lagrangian relaxation implementation in `branch_bound.c` is tightly coupled to SCP. This plan proposes extracting:
1. **Subgradient optimization** - general first-order optimization method
2. **Lagrangian relaxation framework** - problem-agnostic dual bound computation
3. **SCP-specific callbacks** - plugs into the generic framework

This enables reuse for:
- Lagrangian relaxation of other problem classes (traveling salesman, Steiner tree, etc.)
- Subgradient optimization for other non-smooth problems
- Future: projected gradient descent, proximal methods

## Proposed Module Structure

```
ralph/
├── include/
│   ├── optim.h              # NEW: Public API for optimization module
│   ├── optim_subgradient.h  # NEW: Subgradient method interface
│   ├── optim_lagrangian.h   # NEW: Lagrangian relaxation framework
│   └── mip.h                # Updated: uses optim_lagrangian.h
├── src/
│   ├── optim/               # NEW: Optimization submodule
│   │   ├── subgradient.c    # Subgradient optimization
│   │   ├── lagrangian.c     # Generic Lagrangian relaxation
│   │   └── lagrangian_scp.c # SCP-specific implementation
│   └── branch_bound.c       # Simplified: calls optim/ functions
└── tests/
    └── test_optim.c         # NEW: Unit tests for optim module
```

## Phase 1: Generic Subgradient Optimization

### 1.1 Subgradient Interface

**File:** `ralph/include/optim_subgradient.h`

```c
/*
 * Generic subgradient optimization for non-smooth convex maximization:
 *   max f(x)  where f has subgradients
 *
 * User provides:
 *   - Objective and subgradient evaluation (callback)
 *   - Projection to feasible set (callback)
 *   - Step size strategy
 */

/* Step size strategies */
typedef enum {
    RALPH_STEP_CONSTANT,      /* step = alpha */
    RALPH_STEP_DIMINISHING,   /* step = alpha / sqrt(k) */
    RALPH_STEP_POLYAK,        /* step = alpha * (f* - f(x)) / ||g||^2 */
    RALPH_STEP_ADAPTIVE       /* Adjust based on progress */
} RalphStepStrategy;

/* Subgradient optimization parameters */
typedef struct {
    int max_iterations;       /* Maximum iterations */
    double initial_step;      /* Initial step size (alpha) */
    double min_step;          /* Minimum step size before stopping */
    RalphStepStrategy step_strategy;

    /* For Polyak step: upper bound on optimal value */
    double f_star;            /* Upper bound (for maximization) */
    int use_f_star;           /* 1 if f_star is available */

    /* Adaptive parameters */
    int no_improve_limit;     /* Iterations without improvement before halving step */
    double step_decay;        /* Step reduction factor (default 0.5) */

    /* Convergence tolerance */
    double tol;               /* Stop if ||g|| < tol */
} RalphSubgradientParams;

/* Default parameters */
#define RALPH_SUBGRADIENT_PARAMS_DEFAULT { \
    .max_iterations = 500, \
    .initial_step = 2.0, \
    .min_step = 0.001, \
    .step_strategy = RALPH_STEP_ADAPTIVE, \
    .f_star = 0.0, \
    .use_f_star = 0, \
    .no_improve_limit = 30, \
    .step_decay = 0.5, \
    .tol = 1e-8 \
}

/* Callback: evaluate f(x) and compute subgradient g */
typedef double (*RalphSubgradientFunc)(
    const double *x,          /* Current point */
    double *g,                /* Output: subgradient at x */
    void *user_data           /* User context */
);

/* Callback: project x onto feasible set */
typedef void (*RalphProjectFunc)(
    double *x,                /* In/out: point to project */
    int n,                    /* Dimension */
    void *user_data           /* User context */
);

/* Subgradient optimization result */
typedef struct {
    double best_value;        /* Best objective value found */
    double *best_x;           /* Best point (caller owns) */
    int iterations;           /* Total iterations */
    int improvements;         /* Number of times objective improved */
    int status;               /* 0=converged, 1=max_iter, 2=small_step */
} RalphSubgradientResult;

/*
 * Run subgradient optimization.
 *
 * Parameters:
 *   n         - Dimension of x
 *   x0        - Initial point (copied internally)
 *   params    - Optimization parameters
 *   objective - Callback for f(x) and subgradient
 *   project   - Callback for projection (can be NULL for unconstrained)
 *   user_data - Passed to callbacks
 *   result    - Output: optimization result (caller must free result->best_x)
 *
 * Returns:
 *   0 on success, -1 on error.
 */
int ralph_subgradient_optimize(
    int n,
    const double *x0,
    const RalphSubgradientParams *params,
    RalphSubgradientFunc objective,
    RalphProjectFunc project,
    void *user_data,
    RalphSubgradientResult *result
);
```

### 1.2 Implementation

**File:** `ralph/src/optim/subgradient.c`

Key implementation details:
- Maintain best solution found (may not be last iteration)
- Adaptive step size: halve when no improvement for N iterations
- Support Polyak step size when upper bound known
- Thread-safe (no global state)

**Estimated effort:** 150-200 lines

## Phase 2: Generic Lagrangian Relaxation

### 2.1 Lagrangian Interface

**File:** `ralph/include/optim_lagrangian.h`

```c
/*
 * Generic Lagrangian relaxation framework.
 *
 * For a problem:
 *   min c'x  s.t. Ax >= b, x in X
 *
 * The Lagrangian relaxation with multipliers λ >= 0:
 *   L(λ) = min { c'x + λ'(b - Ax) : x in X }
 *        = λ'b + min { (c - A'λ)'x : x in X }
 *
 * The Lagrangian dual: max L(λ) s.t. λ >= 0
 *
 * User provides:
 *   - Subproblem solver: given reduced costs, find optimal x in X
 *   - Cost vector c
 *   - Constraint matrix A (sparse)
 *   - RHS vector b
 */

/* Lagrangian problem type */
typedef enum {
    RALPH_LAGRANGIAN_COVERING,    /* Ax >= b, minimize */
    RALPH_LAGRANGIAN_PACKING,     /* Ax <= b, maximize */
    RALPH_LAGRANGIAN_PARTITIONING /* Ax = b, minimize */
} RalphLagrangianType;

/* Callback: solve Lagrangian subproblem with reduced costs */
typedef double (*RalphLagrangianSubproblem)(
    int n,                    /* Number of variables */
    const double *reduced_cost, /* c - A'λ */
    double *x,                /* Output: optimal x in X */
    void *user_data           /* User context */
);

/* Callback: repair infeasible Lagrangian solution */
typedef double (*RalphLagrangianRepair)(
    int n,                    /* Number of variables */
    const double *x_lagr,     /* Lagrangian solution (possibly infeasible) */
    double *x_feasible,       /* Output: feasible solution */
    void *user_data           /* User context */
);

/* Lagrangian relaxation context */
typedef struct {
    int m;                    /* Number of constraints (dual dimension) */
    int n;                    /* Number of variables (primal dimension) */
    RalphLagrangianType type; /* Problem type */

    /* Problem data */
    const double *c;          /* Objective coefficients [n] */
    const double *b;          /* RHS values [m] */
    const int *A_colptr;      /* CSC column pointers [n+1] */
    const int *A_rowidx;      /* CSC row indices */
    const double *A_values;   /* CSC values (can be NULL for 0-1 matrix) */

    /* Callbacks */
    RalphLagrangianSubproblem subproblem;
    RalphLagrangianRepair repair;  /* Optional */
    void *user_data;

    /* Optimization state */
    double *lambda;           /* Current multipliers [m] */
    double *best_lambda;      /* Best multipliers found [m] */
    double *subgradient;      /* Current subgradient [m] */
    double *x_lagrangian;     /* Current subproblem solution [n] */

    double best_dual_bound;   /* Best Lagrangian bound */
    double best_primal_bound; /* Best feasible solution (from repair) */
    double *best_solution;    /* Best feasible solution [n] */

    /* Parameters */
    RalphSubgradientParams subgrad_params;

    /* Statistics */
    int iterations;
    int bound_improvements;
    int primal_improvements;
} RalphLagrangianContext;

/*
 * Create Lagrangian relaxation context.
 */
RalphLagrangianContext *ralph_lagrangian_create(
    int m, int n,
    RalphLagrangianType type,
    const double *c,
    const double *b,
    const int *A_colptr,
    const int *A_rowidx,
    const double *A_values,   /* Can be NULL for 0-1 matrix */
    RalphLagrangianSubproblem subproblem,
    void *user_data
);

void ralph_lagrangian_free(RalphLagrangianContext *ctx);

/*
 * Set repair heuristic callback.
 */
void ralph_lagrangian_set_repair(
    RalphLagrangianContext *ctx,
    RalphLagrangianRepair repair
);

/*
 * Set initial multipliers (default: zero).
 */
void ralph_lagrangian_set_lambda(
    RalphLagrangianContext *ctx,
    const double *lambda
);

/*
 * Set primal upper bound (for Polyak step size).
 */
void ralph_lagrangian_set_primal_bound(
    RalphLagrangianContext *ctx,
    double ub
);

/*
 * Compute Lagrangian bound for current multipliers.
 * Also updates subgradient and x_lagrangian.
 */
double ralph_lagrangian_bound(RalphLagrangianContext *ctx);

/*
 * Run full Lagrangian optimization.
 *
 * Returns best dual bound found.
 */
double ralph_lagrangian_optimize(RalphLagrangianContext *ctx);

/*
 * Get results.
 */
double ralph_lagrangian_dual_bound(const RalphLagrangianContext *ctx);
double ralph_lagrangian_primal_bound(const RalphLagrangianContext *ctx);
const double *ralph_lagrangian_solution(const RalphLagrangianContext *ctx);
```

### 2.2 Implementation

**File:** `ralph/src/optim/lagrangian.c`

Key implementation details:
- Uses `ralph_subgradient_optimize()` internally
- Computes reduced costs: `c_bar[j] = c[j] - sum(lambda[i] * A[i,j])`
- Calls user's subproblem solver with reduced costs
- Computes subgradient: `g[i] = b[i] - sum(A[i,j] * x[j])`
- Projects multipliers: `lambda[i] = max(0, lambda[i])` for covering/packing

**Estimated effort:** 200-250 lines

## Phase 3: SCP-Specific Implementation

### 3.1 SCP Lagrangian Module

**File:** `ralph/src/optim/lagrangian_scp.c`

```c
/*
 * SCP-specific Lagrangian relaxation using generic framework.
 *
 * Subproblem for SCP:
 *   min (c - A'λ)'x  s.t. x in {0,1}^n
 *
 * Trivial solution: x[j] = 1 if reduced_cost[j] < 0, else 0
 *
 * Repair heuristic: greedy set cover on uncovered elements
 */

/* Subproblem callback: trivial binary selection */
static double scp_subproblem(int n, const double *reduced_cost,
                              double *x, void *user_data) {
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        if (reduced_cost[j] < -RALPH_ZERO_TOL) {
            x[j] = 1.0;
            obj += reduced_cost[j];
        } else {
            x[j] = 0.0;
        }
    }
    return obj;
}

/* Repair callback: greedy cover */
static double scp_repair(int n, const double *x_lagr,
                          double *x_feasible, void *user_data) {
    /* Copy Lagrangian solution, then add sets to cover uncovered elements */
    /* (Implementation similar to existing lagrangian_repair) */
}

/*
 * High-level SCP Lagrangian solver.
 */
int ralph_lagrangian_solve_scp(MIPSolver *solver, double *solution,
                                double *lower_bound);
```

### 3.2 Migration Path

1. Create new optim/ directory and files
2. Implement generic subgradient optimization
3. Implement generic Lagrangian framework
4. Create SCP-specific callbacks
5. Update `branch_bound.c` to use new API
6. Add comprehensive tests
7. Remove old code from `branch_bound.c`

**Estimated effort:** 100-150 lines (mostly wrapper code)

## Phase 4: Additional Optimizers (Future)

### 4.1 Projected Gradient Descent

For smooth constrained problems:
```c
int ralph_projected_gradient(
    int n,
    const double *x0,
    RalphGradientFunc gradient,
    RalphProjectFunc project,
    void *user_data,
    RalphGradientResult *result
);
```

### 4.2 Proximal Gradient Methods

For composite problems f(x) + g(x) where f is smooth, g has proximal operator:
```c
int ralph_proximal_gradient(
    int n,
    const double *x0,
    RalphGradientFunc gradient_f,
    RalphProxFunc prox_g,
    void *user_data,
    RalphProximalResult *result
);
```

### 4.3 Coordinate Descent

For separable problems or large-scale sparse:
```c
int ralph_coordinate_descent(
    int n,
    const double *x0,
    RalphCoordUpdateFunc update,
    void *user_data,
    RalphCDResult *result
);
```

## Testing Strategy

### New Test File: `tests/test_optim.c`

```c
/* Subgradient tests */
test_subgradient_constant_step();
test_subgradient_polyak_step();
test_subgradient_adaptive();
test_subgradient_convergence();

/* Lagrangian tests */
test_lagrangian_create();
test_lagrangian_bound();
test_lagrangian_optimize();
test_lagrangian_repair();

/* SCP integration tests */
test_scp_via_generic_lagrangian();
test_scp_matches_old_implementation();
```

## Implementation Order

| Phase | Component | Priority | Effort | Depends On | Status |
|-------|-----------|----------|--------|------------|--------|
| 1.1 | Subgradient interface | HIGH | 150 lines | - | **Done** |
| 1.2 | Subgradient implementation | HIGH | 195 lines | 1.1 | **Done** |
| 1.3 | Subgradient tests | HIGH | 340 lines | 1.2 | **Done** |
| 2.1 | Lagrangian interface | HIGH | 230 lines | 1.1 | **Done** |
| 2.2 | Lagrangian implementation | HIGH | 340 lines | 1.2, 2.1 | **Done** |
| 2.3 | Lagrangian tests | HIGH | 250 lines | 2.2 | **Done** |
| 3.1 | SCP callbacks | HIGH | 570 lines | 2.2 | **Done** |
| 3.2 | Migration | MEDIUM | -400 lines | 3.1 | **Done** |
| 4.x | Future optimizers | LOW | - | 1.2 | Planned |

**Total estimated effort:** ~700 lines for core functionality

## Benefits

1. **Reusability**: Lagrangian relaxation for TSP, Steiner tree, scheduling, etc.
2. **Maintainability**: Single implementation of subgradient algorithm
3. **Testability**: Each component tested independently
4. **Extensibility**: Easy to add new step size strategies, stopping criteria
5. **Performance**: Callback-based design avoids virtual function overhead

## API Example

```c
/* Using the generic Lagrangian framework for SCP */

/* User's subproblem: trivial binary selection */
double my_subproblem(int n, const double *rc, double *x, void *ctx) {
    double obj = 0;
    for (int j = 0; j < n; j++) {
        x[j] = (rc[j] < 0) ? 1.0 : 0.0;
        if (x[j] > 0.5) obj += rc[j];
    }
    return obj;
}

/* Create context */
RalphLagrangianContext *ctx = ralph_lagrangian_create(
    m, n, RALPH_LAGRANGIAN_COVERING,
    costs, rhs, A->colptr, A->rowidx, NULL,
    my_subproblem, my_data
);

/* Set primal bound from heuristic */
ralph_lagrangian_set_primal_bound(ctx, greedy_solution_cost);

/* Optimize */
double lb = ralph_lagrangian_optimize(ctx);
printf("Lagrangian bound: %.4f\n", lb);

ralph_lagrangian_free(ctx);
```

## File Changes Summary

| File | Action | Lines Changed |
|------|--------|---------------|
| `include/optim.h` | Create | ~30 |
| `include/optim_subgradient.h` | Create | ~100 |
| `include/optim_lagrangian.h` | Create | ~150 |
| `src/optim/subgradient.c` | Create | ~200 |
| `src/optim/lagrangian.c` | Create | ~300 |
| `src/optim/lagrangian_scp.c` | Create | ~150 |
| `src/branch_bound.c` | Modify | -350 (remove old code) |
| `tests/test_optim.c` | Create | ~400 |
| `Makefile` | Modify | ~20 |

**Net change:** ~600 new lines, modular and reusable

## Benchmarking Specialized Solvers

The MIP benchmark suite (`bench_mip.c`) now supports three modes for comparing solver performance:

### Benchmark Modes

| Mode | Command | Description |
|------|---------|-------------|
| Generic | `make bench-mip` | Ralph generic MIP vs GLPK |
| Specialized | `make bench-mip-specialized` | Ralph with LAP/Network/SCP detection vs GLPK |
| Compare All | `make bench-mip-compare` | Side-by-side comparison of all three solvers |

### Quick Benchmark Variants

For faster testing with smaller problem sizes:
```bash
make bench-mip-quick
make bench-mip-specialized-quick
make bench-mip-compare-quick
```

### Command-Line Options

```bash
./bench_mip [options]

Modes:
  (default)           Generic Ralph MIP vs GLPK
  --specialized       Ralph with specialized solvers vs GLPK
  --compare-all       Compare all three: generic, specialized, and GLPK

Options:
  --quick             Run quick benchmarks (smaller sizes)
  --problem NAME      Run only: setcover, setpart, assignment, netflow, facility
  --size N            Override problem size
  --time-limit T      Set time limit in seconds (default: 60)
```

### Problem Types Tested

| Problem | Specialized Solver | Status |
|---------|-------------------|--------|
| Linear Assignment | JVC (LAP) | ✅ **Integrated** - 86-633x faster than MIP |
| Set Covering | SCP optimizations | ✅ **Integrated** - 1.4-4.7x faster |
| Set Partitioning | SCP optimizations | ✅ **Integrated** - uses same SCP code |
| Network Flow (LP) | Network Simplex | ✅ **Integrated** - 1.6-1.8x faster |
| Facility Location | Generic MIP | Baseline comparison |

### What `detect_special=1` Currently Enables

1. **LAP Detection** (MIP): Auto-detects n×n assignment structure, uses JVC algorithm
2. **SCP Detection** (MIP): Auto-detects set covering/partitioning structure, enables:
   - SCP-specific heuristics (greedy + local search)
   - SCP-specific cuts (clique, odd-hole, lifted cover)
   - Lagrangian relaxation for tighter dual bounds
   - SCP-aware pseudo-cost initialization
3. **Network Detection** (LP): Auto-detects network flow structure, uses network simplex

Note: The specialized solver detection (`detect_special=1`) auto-detects problem structure. Random benchmark problems may not always trigger detection if they don't match the expected matrix patterns exactly
