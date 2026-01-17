# Plan: Dual Simplex as Exchangeable Solver Method

## Objective

Make dual simplex available as a solver option, similar to how pricing strategy and presolve work. Users should be able to select between primal and dual simplex via a parameter.

## Current State

### Existing Components

1. **Primal Simplex** (`simplex.c`)
   - `simplex_create(LPModel*)` - Creates solver
   - `simplex_solve(SimplexSolver*)` - Main solve function
   - Has Phase 1 (Big-M) + Phase 2 (optimization)

2. **Dual Simplex** (`dual_simplex.c`)
   - `dual_simplex_solve(SimplexSolver*)` - Already implemented
   - Used only in branch_bound.c for warm-start re-optimization
   - Shares `SimplexSolver` and `SimplexTableau` structures with primal

3. **API** (`ralph.c`)
   - Currently hardcoded to call `simplex_solve()`
   - No method selection parameter exists

### Interface Analysis

Both primal and dual simplex use the same:
- `SimplexSolver` struct
- `SimplexTableau` struct
- `LUFactorization` struct
- Pricing functions
- Solution storage

The key differences:
- **Primal:** Selects entering variable first (pricing), then leaving (ratio test)
- **Dual:** Selects leaving variable first (most infeasible), then entering (dual ratio test)

---

## Implementation Plan

### Phase 1: Add Method Parameter (Non-Breaking)

**Files to modify:**
- `ralph/src/ralph.c`
- `ralph/include/ralph.h` (if enum needed)

**Steps:**

1.1. Add `method` field to `RalphModel` struct:
```c
struct RalphModel {
    // ... existing fields ...
    int method;  // 0=primal, 1=dual, 2=auto
};
```

1.2. Initialize `method` in `ralph_create()`:
```c
model->method = 0;  // Default: primal simplex
```

1.3. Add parameter handling in `ralph_set_int_param()`:
```c
} else if (strcmp(name, "method") == 0 || strcmp(name, "Method") == 0) {
    model->method = value;  // 0=primal, 1=dual, 2=auto
```

1.4. Add parameter retrieval in `ralph_get_int_param()`:
```c
} else if (strcmp(name, "method") == 0) {
    *value = model->method;
```

**Test:** Verify all existing tests pass (parameter added but not used yet).

---

### Phase 2: Wire Up Dual Simplex in Solve Path

**Files to modify:**
- `ralph/src/ralph.c`
- `ralph/src/dual_simplex.c` (minor adjustments)

**Steps:**

2.1. Modify LP solve path in `ralph_optimize()`:
```c
/* Solve */
if (model->method == 1) {
    /* Dual simplex */
    dual_simplex_solve(model->lp_solver);
} else if (model->method == 2) {
    /* Auto: choose based on problem characteristics */
    /* For now, use dual for all-<= constraints, primal otherwise */
    int use_dual = should_use_dual(solve_model);
    if (use_dual) {
        dual_simplex_solve(model->lp_solver);
    } else {
        simplex_solve(model->lp_solver);
    }
} else {
    /* Primal simplex (default) */
    simplex_solve(model->lp_solver);
}
```

2.2. Add helper function to detect when dual is preferred:
```c
static int should_use_dual(LPModel *model) {
    /* Dual simplex is preferred when:
     * - All constraints are <= (slack variables provide dual-feasible start)
     * - Starting from a dual-feasible basis (re-optimization)
     */
    for (int i = 0; i < model->num_cons; i++) {
        if (model->sense[i] != 'L') return 0;
    }
    return 1;
}
```

**Test:** Run comparison test with method=0 vs method=1 vs method=2.

---

### Phase 3: Ensure Dual Simplex Handles All Cases

**Files to modify:**
- `ralph/src/dual_simplex.c`

**Steps:**

3.1. Verify dual simplex handles non-dual-feasible starts:
- Current code falls back to primal if not dual-feasible
- This is correct behavior but should be documented

3.2. Add iteration tracking to dual simplex:
```c
solver->iterations = iter;  // Already exists, verify
```

3.3. Verify solution recovery:
- `solver->solution`, `solver->dual_solution`, `solver->reduced_costs`
- Should be populated by `tableau_compute_solution()`

3.4. Add Phase 1 handling for dual:
- If starting point is not dual-feasible, need to achieve dual feasibility first
- Current implementation falls back to primal - acceptable for now

**Test:** Run all 45 existing tests with method=1.

---

### Phase 4: Optimize Dual Simplex (Performance)

**Files to modify:**
- `ralph/src/dual_simplex.c`

**Steps:**

4.1. Add Devex pricing for dual:
- Dual uses different pricing (dual ratio test)
- Consider adding approximate steepest edge for dual

4.2. Improve dual ratio test:
- Current implementation is basic
- Could add Harris-style tolerances

4.3. Add dual-specific cycling prevention:
- Track degenerate dual pivots
- Implement dual Bland's rule if needed

**Test:** Run 1000x500 benchmark with method=1, compare to method=0 and GLPK.

---

### Phase 5: Documentation and Cleanup

**Files to modify:**
- `ralph/LP_SOLVER_INTERNALS.md`
- `ralph/CLAUDE.md`
- `ralph/include/ralph.h` (add comments)

**Steps:**

5.1. Document the new parameter:
```c
/* Method selection for LP solve:
 * 0 = Primal simplex (default)
 * 1 = Dual simplex
 * 2 = Auto (choose based on problem structure)
 */
```

5.2. Update LP_SOLVER_INTERNALS.md with dual simplex details

5.3. Add to CLAUDE.md quick reference

---

## Testing Strategy

### Per-Phase Testing

| Phase | Test Command | Expected |
|-------|-------------|----------|
| 1 | `make test-ralph` | All 45 pass |
| 2 | `make test-ralph` + benchmark | All pass, dual may be faster |
| 3 | `make test-ralph` with method=1 | All 45 pass |
| 4 | Benchmark comparison | Dual ~2-3x fewer iterations |

### Benchmark Test

Create `/tmp/test_method.c`:
```c
// Compare method=0 vs method=1 vs GLPK
// On 1000x500, 30% density
// Measure iterations and time
```

### Edge Cases to Test

1. Infeasible LP with method=1
2. Unbounded LP with method=1
3. Degenerate LP with method=1
4. LP with equality constraints (dual needs artificial handling)
5. LP with >= constraints (dual starts infeasible)

---

## Rollback Plan

If any phase breaks tests:
1. `git checkout HEAD~1` to revert last commit
2. Investigate failure
3. Fix and re-commit

Each phase is a separate commit for easy rollback.

---

## File Change Summary

| File | Phase | Changes |
|------|-------|---------|
| `ralph/src/ralph.c` | 1, 2 | Add method param, wire up solve path |
| `ralph/include/ralph.h` | 1 | Add method constants (optional) |
| `ralph/src/dual_simplex.c` | 3, 4 | Improve handling, add Devex |
| `ralph/LP_SOLVER_INTERNALS.md` | 5 | Documentation |
| `ralph/CLAUDE.md` | 5 | Quick reference |

---

## Success Criteria

1. All 45 existing tests pass with default method (0)
2. All 45 tests pass with method=1
3. method=1 uses fewer iterations than method=0 on all-<= LPs
4. method=2 auto-selects appropriately
5. No regression in MIP solver (still uses dual for warm starts)
