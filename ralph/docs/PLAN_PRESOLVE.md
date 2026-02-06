# Implementation Plan: LP Presolve Module

> **Part of**: [LP Performance Plan](LP_PERFORMANCE_PLAN.md)
> **Status**: Not Started
> **Priority**: High - required to solve beaconfd and improve performance on large LPs
> **Estimated effort**: 3-4 weeks total

## Overview

Presolve is a preprocessing phase that simplifies LP/MIP problems before solving. It reduces problem size, improves numerical conditioning, and can detect infeasibility/unboundedness early. All production solvers (CPLEX, Gurobi, HiGHS, CLP) include presolve.

### Why Presolve?

| Benefit | Example |
|---------|---------|
| **Smaller problems** | Remove fixed vars, redundant rows → fewer iterations |
| **Better numerics** | Tighter bounds → smaller coefficients → less rounding error |
| **Early detection** | Find infeasibility before expensive simplex iterations |
| **Solve beaconfd** | Detect and remove 140 redundant equality constraints |

### Target Problem: beaconfd

beaconfd has 173 constraints with 140 equalities. The constraint matrix is rank-deficient - some rows are linear combinations of others. Current behavior:
- Two-phase simplex Phase 1 succeeds (feasibility found)
- Phase 2 fails because basis matrix is singular
- Root cause: redundant constraints, not numerical error

With presolve:
- Detect rank deficiency during preprocessing
- Remove redundant rows
- Pass reduced problem to simplex
- Simplex operates on full-rank matrix

## Architecture

### Design Principles

1. **Standalone module**: `src/presolve.c` with its own header `include/presolve.h`
2. **Non-destructive**: Creates a reduced model, preserves original
3. **Reversible**: Postsolve restores solution to original variable space
4. **Incremental**: Each technique can be enabled/disabled independently
5. **Default on**: Integrated into `ralph_optimize()` by default

### Data Flow

```
Original Model                    Presolve Module                    Solver
    │                                   │                               │
    │  ralph_presolve_create(model)     │                               │
    ├──────────────────────────────────►│                               │
    │                                   │                               │
    │   PresolveResult {                │                               │
    │     reduced_model,                │                               │
    │     postsolve_info,               │                               │
    │     reductions[]                  │                               │
    │   }                               │                               │
    │◄──────────────────────────────────┤                               │
    │                                   │                               │
    │                                   │  simplex_solve(reduced_model) │
    │                                   ├──────────────────────────────►│
    │                                   │                               │
    │                                   │  solution (reduced space)     │
    │                                   │◄──────────────────────────────┤
    │                                   │                               │
    │  ralph_postsolve(result, sol)     │                               │
    ├──────────────────────────────────►│                               │
    │                                   │                               │
    │  solution (original space)        │                               │
    │◄──────────────────────────────────┤                               │
```

### Module Structure

```
ralph/
├── include/
│   └── presolve.h          # Public API
├── src/
│   └── presolve.c          # Implementation
└── tests/
    └── test_presolve.c     # Unit tests
```

## Public API

```c
/* include/presolve.h */

#ifndef RALPH_PRESOLVE_H
#define RALPH_PRESOLVE_H

#include "ralph.h"

/* Presolve configuration */
typedef struct {
    int detect_redundant_rows;      /* Default: 1 - detect linearly dependent rows */
    int bound_tightening;           /* Default: 1 - derive tighter variable bounds */
    int fixed_var_removal;          /* Default: 1 - substitute variables with lb==ub */
    int singleton_row_removal;      /* Default: 1 - rows with single variable */
    int singleton_col_removal;      /* Default: 1 - columns appearing in single row */
    int dominated_row_removal;      /* Default: 1 - remove implied constraints */
    int coefficient_reduction;      /* Default: 0 - experimental */
    int max_passes;                 /* Default: 10 - limit presolve iterations */
    int max_time_ms;                /* Default: 1000 - time limit for presolve */
    int verbose;                    /* Default: 0 - print reduction stats */
} RalphPresolveConfig;

/* Default configuration */
#define RALPH_PRESOLVE_CONFIG_DEFAULT { \
    .detect_redundant_rows = 1,     \
    .bound_tightening = 1,          \
    .fixed_var_removal = 1,         \
    .singleton_row_removal = 1,     \
    .singleton_col_removal = 1,     \
    .dominated_row_removal = 1,     \
    .coefficient_reduction = 0,     \
    .max_passes = 10,               \
    .max_time_ms = 1000,            \
    .verbose = 0                    \
}

/* Reduction types (for postsolve stack) */
typedef enum {
    RALPH_REDUCE_FIXED_VAR,         /* Variable fixed to value */
    RALPH_REDUCE_REDUNDANT_ROW,     /* Row removed (linearly dependent) */
    RALPH_REDUCE_SINGLETON_ROW,     /* Single-variable row → bound update */
    RALPH_REDUCE_SINGLETON_COL,     /* Single-row variable → substitution */
    RALPH_REDUCE_DOMINATED_ROW,     /* Implied constraint removed */
    RALPH_REDUCE_BOUND_CHANGE,      /* Variable bound tightened */
    RALPH_REDUCE_EMPTY_ROW,         /* All-zero row removed */
    RALPH_REDUCE_EMPTY_COL          /* All-zero column removed */
} RalphReductionType;

/* Presolve statistics */
typedef struct {
    int rows_removed;               /* Total rows eliminated */
    int cols_removed;               /* Total columns eliminated */
    int bounds_tightened;           /* Number of bound improvements */
    int redundant_rows;             /* Linearly dependent rows found */
    int fixed_vars;                 /* Variables with lb==ub */
    int singleton_rows;             /* Single-variable rows */
    int singleton_cols;             /* Single-row columns */
    int passes;                     /* Number of presolve passes */
    double time_ms;                 /* Time spent in presolve */
} RalphPresolveStats;

/* Presolve result (opaque, contains postsolve info) */
typedef struct RalphPresolveResult RalphPresolveResult;

/* Main API */

/**
 * Run presolve on a model.
 *
 * @param model     Original LP/MIP model
 * @param config    Presolve configuration (NULL for defaults)
 * @return          Presolve result containing reduced model, or NULL on error
 *
 * The original model is NOT modified. The result contains:
 * - A reduced model suitable for solving
 * - Postsolve information to restore the solution
 * - Statistics about reductions applied
 */
RalphPresolveResult* ralph_presolve(RalphModel *model,
                                    const RalphPresolveConfig *config);

/**
 * Get the reduced model from presolve result.
 *
 * @param result    Presolve result from ralph_presolve()
 * @return          Reduced model (owned by result, do not free separately)
 */
RalphModel* ralph_presolve_get_model(RalphPresolveResult *result);

/**
 * Get presolve statistics.
 *
 * @param result    Presolve result
 * @param stats     Output statistics structure
 */
void ralph_presolve_get_stats(const RalphPresolveResult *result,
                              RalphPresolveStats *stats);

/**
 * Run postsolve to restore solution to original variable space.
 *
 * @param result    Presolve result containing postsolve info
 * @param reduced_x Solution values for reduced model (size = reduced num_vars)
 * @param original_x Output: solution values for original model (size = original num_vars)
 * @return          0 on success, -1 on error
 *
 * This reverses all presolve reductions to compute variable values
 * for the original model from the reduced model's solution.
 */
int ralph_postsolve(const RalphPresolveResult *result,
                    const double *reduced_x,
                    double *original_x);

/**
 * Free presolve result and reduced model.
 *
 * @param result    Presolve result to free
 */
void ralph_presolve_free(RalphPresolveResult *result);

/**
 * Check if problem is infeasible based on presolve.
 *
 * @param result    Presolve result
 * @return          1 if proven infeasible, 0 otherwise
 */
int ralph_presolve_is_infeasible(const RalphPresolveResult *result);

/**
 * Check if problem is unbounded based on presolve.
 *
 * @param result    Presolve result
 * @return          1 if proven unbounded, 0 otherwise
 */
int ralph_presolve_is_unbounded(const RalphPresolveResult *result);

#endif /* RALPH_PRESOLVE_H */
```

### Integration with ralph_optimize()

```c
/* In ralph.c - default behavior */
RalphStatus ralph_optimize(RalphModel *model) {
    /* Check presolve parameter (default: enabled) */
    int do_presolve = ralph_get_int_param(model, "presolve");
    if (do_presolve < 0) do_presolve = 1;  /* Default on */

    if (do_presolve) {
        RalphPresolveConfig config = RALPH_PRESOLVE_CONFIG_DEFAULT;
        RalphPresolveResult *presolved = ralph_presolve(model, &config);

        if (ralph_presolve_is_infeasible(presolved)) {
            ralph_presolve_free(presolved);
            return RALPH_INFEASIBLE;
        }

        RalphModel *reduced = ralph_presolve_get_model(presolved);
        RalphStatus status = simplex_solve_internal(reduced);

        if (status == RALPH_OPTIMAL) {
            /* Postsolve to get original variable values */
            ralph_postsolve(presolved, reduced->solution, model->solution);
        }

        ralph_presolve_free(presolved);
        return status;
    } else {
        return simplex_solve_internal(model);
    }
}
```

## Implementation Phases

### Phase 1: Redundant Row Detection (Priority: Critical)

**Goal**: Solve beaconfd by detecting and removing linearly dependent constraints.

**Algorithm**: Gaussian elimination on constraint matrix to find rank

```c
/**
 * Detect redundant rows via rank computation.
 *
 * Algorithm:
 * 1. Copy constraint matrix to dense workspace
 * 2. Perform Gaussian elimination with partial pivoting
 * 3. Rows that reduce to all-zeros are redundant
 * 4. Check consistency: if b[row] != 0 for zero row → infeasible
 *
 * Complexity: O(m * n * min(m,n)) for dense, less for sparse
 */
static int detect_redundant_rows(PresolveState *state) {
    int m = state->num_rows;
    int n = state->num_cols;

    /* Work in column-major dense matrix for numerical stability */
    double *A = calloc(m * n, sizeof(double));
    double *b = malloc(m * sizeof(double));
    /* ... copy from sparse ... */

    int rank = 0;
    int *pivot_row = malloc(n * sizeof(int));

    for (int col = 0; col < n && rank < m; col++) {
        /* Find pivot */
        int best = -1;
        double best_val = RALPH_PIVOT_TOL;
        for (int row = rank; row < m; row++) {
            double val = fabs(A[row + col * m]);
            if (val > best_val) {
                best_val = val;
                best = row;
            }
        }

        if (best < 0) continue;  /* No pivot in this column */

        /* Swap rows */
        if (best != rank) {
            for (int j = col; j < n; j++) {
                double tmp = A[rank + j * m];
                A[rank + j * m] = A[best + j * m];
                A[best + j * m] = tmp;
            }
            double tmp = b[rank];
            b[rank] = b[best];
            b[best] = tmp;
        }

        /* Eliminate */
        double pivot = A[rank + col * m];
        for (int row = rank + 1; row < m; row++) {
            double factor = A[row + col * m] / pivot;
            if (fabs(factor) < RALPH_ZERO_TOL) continue;

            for (int j = col; j < n; j++) {
                A[row + j * m] -= factor * A[rank + j * m];
            }
            b[row] -= factor * b[rank];
        }

        pivot_row[rank] = col;
        rank++;
    }

    /* Rows rank..m-1 are redundant */
    for (int row = rank; row < m; row++) {
        if (fabs(b[row]) > RALPH_FEAS_TOL) {
            /* Inconsistent: 0 = b[row] ≠ 0 */
            state->proven_infeasible = 1;
            return -1;
        }
        /* Mark row as redundant */
        mark_row_removed(state, row, RALPH_REDUCE_REDUNDANT_ROW);
    }

    state->stats.redundant_rows = m - rank;
    return 0;
}
```

**TODO Items for Phase 1:**

- [ ] Create `include/presolve.h` with API declarations
- [ ] Create `src/presolve.c` with basic structure
- [ ] Implement `RalphPresolveResult` struct with postsolve stack
- [ ] Implement `ralph_presolve()` main entry point
- [ ] Implement `detect_redundant_rows()` using Gaussian elimination
- [ ] Implement postsolve for redundant row removal (trivial - just skip)
- [ ] Add `ralph_presolve_free()`
- [ ] Create `tests/test_presolve.c` with basic tests
- [ ] Test on beaconfd specifically
- [ ] Integrate into `ralph_optimize()` with parameter control
- [ ] Update CLAUDE.md with presolve API documentation

**Estimated effort**: 3-4 days

---

### Phase 2: Fixed Variable Removal

**Goal**: Remove variables where lower bound equals upper bound.

```c
/**
 * Remove fixed variables (lb == ub).
 *
 * For each variable j with lb[j] == ub[j] == v:
 * 1. Substitute x[j] = v into all constraints
 * 2. Update RHS: b[i] -= A[i,j] * v
 * 3. Update objective offset: obj_offset += c[j] * v
 * 4. Remove column j from matrix
 * 5. Record substitution for postsolve
 */
static int remove_fixed_variables(PresolveState *state) {
    for (int j = 0; j < state->num_cols; j++) {
        if (state->col_removed[j]) continue;

        double lb = state->lb[j];
        double ub = state->ub[j];

        if (fabs(ub - lb) < RALPH_ZERO_TOL) {
            double value = lb;

            /* Update RHS for all rows containing this variable */
            for (int k = state->A->colptr[j]; k < state->A->colptr[j+1]; k++) {
                int row = state->A->rowidx[k];
                if (state->row_removed[row]) continue;
                state->b[row] -= state->A->values[k] * value;
            }

            /* Update objective offset */
            state->obj_offset += state->c[j] * value;

            /* Record for postsolve */
            record_reduction(state, RALPH_REDUCE_FIXED_VAR, j, value);
            mark_col_removed(state, j);
            state->stats.fixed_vars++;
        }
    }
    return 0;
}
```

**TODO Items for Phase 2:**

- [ ] Implement `remove_fixed_variables()` in presolve.c
- [ ] Add postsolve handler: `original_x[j] = recorded_value`
- [ ] Add tests for fixed variable removal
- [ ] Test interaction with redundant row detection

**Estimated effort**: 1 day

---

### Phase 3: Singleton Row/Column Removal

**Goal**: Simplify constraints with single non-zero coefficient.

#### Singleton Rows

A row with single non-zero `a[i,j]` implies a bound on `x[j]`:
- `a[i,j] * x[j] <= b[i]` → `x[j] <= b[i]/a[i,j]` (if a > 0)
- `a[i,j] * x[j] >= b[i]` → `x[j] >= b[i]/a[i,j]` (if a > 0)
- `a[i,j] * x[j] = b[i]` → `x[j] = b[i]/a[i,j]` (fixed variable)

```c
static int remove_singleton_rows(PresolveState *state) {
    for (int i = 0; i < state->num_rows; i++) {
        if (state->row_removed[i]) continue;

        /* Count non-zeros in row */
        int nnz = 0, singleton_col = -1;
        double coef = 0;
        for (int j = 0; j < state->num_cols; j++) {
            double val = get_coefficient(state, i, j);
            if (fabs(val) > RALPH_ZERO_TOL) {
                nnz++;
                singleton_col = j;
                coef = val;
                if (nnz > 1) break;
            }
        }

        if (nnz == 0) {
            /* Empty row: check feasibility */
            if (state->sense[i] == 'L' && state->b[i] < -RALPH_FEAS_TOL) {
                state->proven_infeasible = 1;
                return -1;
            }
            /* ... similar for 'G' and 'E' ... */
            mark_row_removed(state, i, RALPH_REDUCE_EMPTY_ROW);
        }
        else if (nnz == 1) {
            /* Singleton row → bound update */
            double implied = state->b[i] / coef;
            int updated = 0;

            if (state->sense[i] == 'E') {
                /* Equality: fix variable */
                state->lb[singleton_col] = implied;
                state->ub[singleton_col] = implied;
                updated = 1;
            }
            else if ((state->sense[i] == 'L' && coef > 0) ||
                     (state->sense[i] == 'G' && coef < 0)) {
                /* Upper bound */
                if (implied < state->ub[singleton_col]) {
                    state->ub[singleton_col] = implied;
                    updated = 1;
                }
            }
            else {
                /* Lower bound */
                if (implied > state->lb[singleton_col]) {
                    state->lb[singleton_col] = implied;
                    updated = 1;
                }
            }

            if (updated) {
                record_reduction(state, RALPH_REDUCE_SINGLETON_ROW, i, 0);
                mark_row_removed(state, i, RALPH_REDUCE_SINGLETON_ROW);
                state->stats.singleton_rows++;
            }
        }
    }
    return 0;
}
```

#### Singleton Columns

A column appearing in only one constraint can sometimes be substituted out or bounded by the constraint.

```c
static int remove_singleton_columns(PresolveState *state) {
    for (int j = 0; j < state->num_cols; j++) {
        if (state->col_removed[j]) continue;

        /* Count non-zeros in column */
        int nnz = 0, singleton_row = -1;
        double coef = 0;
        for (int k = state->A->colptr[j]; k < state->A->colptr[j+1]; k++) {
            int row = state->A->rowidx[k];
            if (state->row_removed[row]) continue;
            if (fabs(state->A->values[k]) > RALPH_ZERO_TOL) {
                nnz++;
                singleton_row = row;
                coef = state->A->values[k];
                if (nnz > 1) break;
            }
        }

        if (nnz == 0) {
            /* Free column: check objective */
            if (state->c[j] > RALPH_ZERO_TOL && state->ub[j] == RALPH_INFINITY) {
                /* Minimizing positive cost with no upper bound → unbounded */
                state->proven_unbounded = 1;
                return -1;
            }
            /* Variable takes optimal bound value */
            double value = (state->c[j] >= 0) ? state->lb[j] : state->ub[j];
            record_reduction(state, RALPH_REDUCE_EMPTY_COL, j, value);
            mark_col_removed(state, j);
        }
        else if (nnz == 1 && state->sense[singleton_row] != 'E') {
            /* Singleton column in inequality: may be able to fix */
            /* ... complex logic based on objective sign, bound, constraint sense ... */
        }
    }
    return 0;
}
```

**TODO Items for Phase 3:**

- [ ] Implement `remove_singleton_rows()`
- [ ] Implement `remove_singleton_columns()`
- [ ] Handle empty rows (feasibility check)
- [ ] Handle empty columns (unboundedness/optimal bound)
- [ ] Add postsolve handlers for bound derivations
- [ ] Add tests for singleton reductions
- [ ] Test on problems with many singletons

**Estimated effort**: 2 days

---

### Phase 4: Bound Tightening

**Goal**: Derive tighter bounds from constraint implications.

```c
/**
 * Tighten variable bounds from constraint coefficients.
 *
 * For constraint: sum(a[j] * x[j]) <= b
 *
 * Upper bound on x[k]: x[k] <= (b - sum_{j≠k} a[j]*lb[j]) / a[k]  (if a[k] > 0)
 * Lower bound on x[k]: x[k] >= (b - sum_{j≠k} a[j]*ub[j]) / a[k]  (if a[k] > 0)
 *
 * Iterate until no improvement.
 */
static int tighten_bounds(PresolveState *state) {
    int improved = 1;
    int pass = 0;

    while (improved && pass < state->config->max_passes) {
        improved = 0;
        pass++;

        for (int i = 0; i < state->num_rows; i++) {
            if (state->row_removed[i]) continue;

            /* Compute activity bounds for this row */
            double min_activity = 0, max_activity = 0;
            for (/* each j in row i */) {
                double a = get_coefficient(state, i, j);
                if (a > 0) {
                    min_activity += a * state->lb[j];
                    max_activity += a * state->ub[j];
                } else {
                    min_activity += a * state->ub[j];
                    max_activity += a * state->lb[j];
                }
            }

            /* Derive bounds for each variable */
            for (/* each j in row i */) {
                double a = get_coefficient(state, i, j);
                if (fabs(a) < RALPH_ZERO_TOL) continue;

                /* ... compute implied bounds ... */
                /* ... update if tighter ... */
                /* ... record RALPH_REDUCE_BOUND_CHANGE ... */
            }
        }
    }

    return 0;
}
```

**TODO Items for Phase 4:**

- [ ] Implement `tighten_bounds()` with activity computation
- [ ] Handle infinite bounds correctly
- [ ] Detect infeasibility from empty bound intervals
- [ ] Record bound changes for postsolve (informational only)
- [ ] Add tests for bound tightening
- [ ] Benchmark impact on NETLIB problems

**Estimated effort**: 2 days

---

### Phase 5: Dominated Row Removal

**Goal**: Remove constraints implied by others.

A constraint is dominated if it's always satisfied when other constraints are satisfied. For example:
- `x + y <= 10` dominates `x + y <= 20`
- Combined bounds might dominate a constraint

```c
/**
 * Remove dominated (implied) constraints.
 *
 * For each inequality constraint i:
 * 1. Compute max possible LHS from variable bounds
 * 2. If max LHS <= RHS, constraint is always satisfied → remove
 *
 * For pairs of parallel constraints:
 * - Keep only the tighter one
 */
static int remove_dominated_rows(PresolveState *state) {
    for (int i = 0; i < state->num_rows; i++) {
        if (state->row_removed[i]) continue;
        if (state->sense[i] == 'E') continue;  /* Can't remove equalities */

        double max_lhs = compute_max_activity(state, i);
        double min_lhs = compute_min_activity(state, i);

        if (state->sense[i] == 'L' && max_lhs <= state->b[i] + RALPH_FEAS_TOL) {
            /* Constraint always satisfied */
            mark_row_removed(state, i, RALPH_REDUCE_DOMINATED_ROW);
            state->stats.rows_removed++;
        }
        else if (state->sense[i] == 'G' && min_lhs >= state->b[i] - RALPH_FEAS_TOL) {
            /* Constraint always satisfied */
            mark_row_removed(state, i, RALPH_REDUCE_DOMINATED_ROW);
            state->stats.rows_removed++;
        }
    }
    return 0;
}
```

**TODO Items for Phase 5:**

- [ ] Implement `remove_dominated_rows()` with activity bounds
- [ ] Implement parallel row detection and merging
- [ ] Add postsolve (trivial - constraint was redundant)
- [ ] Add tests for dominated row removal
- [ ] Test on problems with redundant inequalities

**Estimated effort**: 1-2 days

---

### Phase 6: Advanced Techniques (Future)

These are lower priority but can significantly help on specific problem types:

#### Probing (MIP only)

Fix binary variables temporarily and propagate implications:
```
If x[j] = 0 implies infeasibility → x[j] must be 1
If x[j] = 0 implies y[k] = 1 → add implication x[j] >= y[k]
```

#### Clique Detection (MIP only)

Find sets of binary variables where at most one can be 1:
```
x[1] + x[2] <= 1
x[2] + x[3] <= 1
→ Clique {x[1], x[2], x[3]}: at most one = 1
```

#### Coefficient Reduction

Strengthen constraint coefficients while preserving feasibility.

**TODO Items for Phase 6:**

- [ ] Design probing framework for binary variables
- [ ] Implement clique detection
- [ ] Implement coefficient strengthening
- [ ] Add MIP-specific presolve tests

**Estimated effort**: 1-2 weeks (future work)

---

## Testing Plan

### Unit Tests (`tests/test_presolve.c`)

```c
/* Phase 1: Redundant rows */
void test_redundant_row_detection(void);
void test_infeasible_redundant_system(void);
void test_beaconfd_rank_deficiency(void);

/* Phase 2: Fixed variables */
void test_fixed_variable_removal(void);
void test_postsolve_fixed_vars(void);

/* Phase 3: Singletons */
void test_singleton_row_to_bound(void);
void test_singleton_column_removal(void);
void test_empty_row_feasibility(void);
void test_empty_column_unboundedness(void);

/* Phase 4: Bound tightening */
void test_bound_tightening_simple(void);
void test_bound_tightening_iteration(void);
void test_bound_infeasibility_detection(void);

/* Phase 5: Dominated rows */
void test_dominated_constraint_removal(void);
void test_parallel_constraint_merging(void);

/* Integration */
void test_presolve_full_pipeline(void);
void test_presolve_netlib_beaconfd(void);
void test_presolve_disabled(void);
void test_postsolve_correctness(void);
```

### NETLIB Regression

After each phase, verify:
1. All currently passing problems still pass
2. Solution quality unchanged or improved
3. Solve time reduced (presolve + solve < original solve)

### Specific Targets

| Problem | Issue | Expected Fix |
|---------|-------|--------------|
| beaconfd | 140 redundant equalities | Phase 1: rank detection |
| (TBD) | Many fixed variables | Phase 2: fixed var removal |
| (TBD) | Loose bounds | Phase 4: bound tightening |

## Success Criteria

| Metric | Current | Target |
|--------|---------|--------|
| NETLIB tiny suite | 4/5 pass | 5/5 pass |
| beaconfd | FAIL | PASS |
| Presolve overhead | N/A | <5% of solve time |
| Unit tests | N/A | >30 tests |

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Postsolve bugs | Extensive round-trip testing |
| Numerical instability in rank detection | Use robust pivoting, configurable tolerance |
| Presolve slower than solving | Time limit, early termination |
| Regressions on working problems | Full NETLIB regression suite |

## References

- Andersen & Andersen, "Presolving in Linear Programming" (1995)
- Achterberg, "Constraint Integer Programming" (2007), Chapter 3
- HiGHS presolve: https://github.com/ERGO-Code/HiGHS/tree/master/src/presolve
- CLP presolve: https://github.com/coin-or/Clp/blob/master/src/ClpPresolve.cpp
