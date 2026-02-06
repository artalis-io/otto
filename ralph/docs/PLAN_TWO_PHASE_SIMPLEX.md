# Implementation Plan: Two-Phase Simplex and Numerical Stability

> **Part of**: [LP Performance Plan](LP_PERFORMANCE_PLAN.md)
> **Status**: Phase 1 Implemented (two-phase infrastructure complete, beaconfd still failing)
> **Priority**: High - required to pass NETLIB tiny suite

## Problem Statement

The Big-M method fails on problems with many equality constraints (e.g., beaconfd: 140 equalities out of 173 constraints). After ~163 iterations, the basis matrix becomes singular due to:

1. Large condition numbers from Big-M costs (1e8)
2. Accumulated numerical error in LU updates
3. Near-linear dependence between columns

### Current NETLIB Results (February 2026)
| Problem | Equalities | Total Constraints | Status |
|---------|------------|-------------------|--------|
| kb2 | 0 | 5 | ✅ PASS |
| share2b | 13 | 96 | ✅ PASS |
| adlittle | 15 | 56 | ✅ PASS |
| bnl1 | 232 | 643 | ✅ PASS (0.009% error) |
| beaconfd | **140** | 173 | ❌ FAIL (see below) |

**Note**: bnl1's equality count was incorrectly listed as 9 in the original plan; the actual count is 232.

## Implementation Status

### ✅ Phase 1: Two-Phase Infrastructure (Complete)

Two-phase simplex is now implemented but only triggers for problems with > 80% equality constraints:

**Files modified:**
- `src/simplex.c`: Added `simplex_phase1()`, `simplex_transition_phase2()`
- `include/lp.h`: Added `use_two_phase`, `c_original`, `artificial_vars`, `num_artificial`, `num_equalities`

**Implementation:**
1. Count equality constraints during tableau creation
2. If equalities > 80% of constraints, use two-phase method
3. Phase 1: Minimize sum of artificial variables (cost = 1.0 each)
4. Transition: Fix non-basic artificials at zero (RALPH_FIXED status)
5. Phase 2: Continue with original objective

**Current issue with beaconfd:**
- Phase 1 completes successfully (158 iterations)
- 10 artificial variables remain stuck in basis at zero (redundant rows)
- Phase 2 refactorization fails at iteration 19 due to numerical issues from stuck artificials

### 🔄 Next Steps: Handle Stuck Artificials

The remaining issue is handling artificial variables that cannot be pivoted out of the basis:

**Option A: Redundant Row Detection**
- Identify rows where artificial is stuck (basis position)
- Detect and remove redundant rows before Phase 2
- Requires modifying constraint matrix structure

**Option B: Improved Pivot Selection**
- Use more aggressive search for replacement variables
- Try multiple candidates per stuck artificial
- Fall back to Big-M if too many stuck

**Option C: Numerical Refinement**
- Use tighter tolerances when artificials are in basis
- More frequent refactorization
- Better condition monitoring

The pattern is clear: problems with a high ratio of equality constraints fail.

## Proposed Solutions

### Phase 1: True Two-Phase Simplex (High Impact)

Replace Big-M method with classic two-phase simplex for problems with equality constraints.

**Current approach (Big-M):**
```
min c'x + M*sum(artificials)
s.t. Ax + artificials = b
```

**Two-phase approach:**
```
Phase 1: min sum(artificials)
         s.t. Ax + artificials = b

Phase 2: min c'x  (drop artificials, use Phase 1 basis)
         s.t. Ax = b
```

**Implementation steps:**

1. **Detect when two-phase is needed** (`simplex.c`)
   - Count equality constraints
   - If equalities > 20% of constraints OR equalities > 50, use two-phase
   - Add parameter `solver->use_two_phase` (auto/always/never)

2. **Create Phase 1 tableau** (`simplex.c:tableau_create_phase1()`)
   - Same structure as current tableau
   - Objective: sum of artificial variables (no Big-M)
   - Artificial variables for '=' and '>=' constraints
   - No artificial variables for '<=' (just slack)

3. **Run Phase 1 simplex** (`simplex.c:simplex_phase1_optimize()`)
   - Standard primal simplex with Phase 1 objective
   - Terminate when objective = 0 (feasible) or optimal > 0 (infeasible)
   - Track which artificials left the basis

4. **Transition to Phase 2** (`simplex.c:simplex_transition_phase2()`)
   - Remove artificial variables from basis (if any remain at zero)
   - Switch objective to original c
   - Recompute reduced costs
   - Continue with Phase 2 basis

5. **Handle degeneracy in transition**
   - If artificial variable is basic at zero, replace with eligible non-basic
   - Use anti-cycling rules during transition

**Files to modify:**
- `src/simplex.c` - Main implementation
- `include/lp.h` - Add `use_two_phase` parameter
- `src/ralph.c` - Expose parameter via API

**Estimated effort:** 3-4 days

---

### Phase 2: Equilibration Scaling (Medium Impact)

Current scaling is basic. Implement equilibration scaling specifically for ill-conditioned problems.

**Algorithm:**
```c
// Iterative row/column scaling to make max element ~1
for (iter = 0; iter < 10; iter++) {
    // Row scaling: divide each row by its max element
    for (i = 0; i < m; i++) {
        row_scale[i] = 1.0 / max_abs_in_row(A, i);
        scale_row(A, i, row_scale[i]);
        b[i] *= row_scale[i];
    }

    // Column scaling: divide each column by its max element
    for (j = 0; j < n; j++) {
        col_scale[j] = 1.0 / max_abs_in_col(A, j);
        scale_col(A, j, col_scale[j]);
        c[j] *= col_scale[j];
        lb[j] /= col_scale[j];
        ub[j] /= col_scale[j];
    }

    if (converged) break;
}
```

**Implementation steps:**

1. **Add equilibration scaling** (`src/simplex.c:apply_equilibration()`)
   - Iterative row/column scaling
   - Track cumulative scale factors
   - Stop when max element is within [0.1, 10]

2. **Detect ill-conditioned problems**
   - Compute initial condition estimate: max(|A|) / min(|A|)
   - If ratio > 1e6, apply aggressive scaling

3. **Scale RHS and bounds consistently**
   - b[i] *= row_scale[i]
   - lb[j] /= col_scale[j], ub[j] /= col_scale[j]

4. **Unscale solution**
   - x[j] *= col_scale[j]
   - y[i] /= row_scale[i]

**Files to modify:**
- `src/simplex.c` - Scaling functions
- Existing `apply_scaling()` / `unscale_solution()` need enhancement

**Estimated effort:** 1-2 days

---

### Phase 3: Intelligent Basis Repair (Medium Impact)

Current repair is naive (swap with slacks). Implement smarter repair.

**Algorithm:**

1. **Identify singular column during factorization**
   - LU factorization fails at step k when pivot is too small
   - Column k of the basis is (nearly) linearly dependent

2. **Find replacement column**
   - Compute which non-basic columns have non-zero entry in row k
   - Pick the one with largest absolute value
   - Prefer slacks > artificials > structural variables

3. **Repair with targeted swap**
   ```c
   // In lu_factorize_dense, when pivot fails:
   *failed_col = k;
   return -1;

   // In repair_singular_basis:
   int failed_col;
   if (lu_factorize_with_diagnosis(lu, B, &failed_col) != 0) {
       // Find best replacement for basis[failed_col]
       replacement = find_best_replacement(tab, failed_col);
       swap_basis(tab, failed_col, replacement);
   }
   ```

4. **Track condition number**
   - After each refactorization, estimate condition number
   - If condition > 1e10, proactively restart with slack basis

**Implementation steps:**

1. **Modify LU to report failure point** (`src/lu.c`)
   - Add `lu_factorize_with_diagnosis()` that returns failed column
   - Track which column caused singularity

2. **Improve `repair_singular_basis()`** (`src/simplex.c`)
   - Use failure point to make targeted repair
   - Try multiple candidates, pick best
   - Track repair history to avoid cycles

3. **Add condition monitoring** (`src/lu.c`)
   - Estimate condition from diagonal of U
   - `cond_estimate = max(|U_ii|) / min(|U_ii|)`
   - Expose via `lu_get_condition_estimate()`

4. **Proactive basis refresh** (`src/simplex.c`)
   - If condition estimate > threshold, force refactorization
   - If still bad, restart with crash basis

**Files to modify:**
- `src/lu.c` - Diagnosis and condition tracking
- `src/simplex.c` - Improved repair logic
- `include/lp.h` - New LU function declarations

**Estimated effort:** 2-3 days

---

## Implementation Order

1. **Two-Phase Simplex** (highest impact, solves root cause)
2. **Equilibration Scaling** (improves all problems, quick win)
3. **Intelligent Basis Repair** (defense in depth)

## Testing Plan

1. **Unit tests for each feature**
   - Two-phase: test transition, degeneracy handling
   - Scaling: test scale/unscale round-trip
   - Repair: test targeted swap logic

2. **NETLIB benchmark regression**
   - Currently: 5/12 pass (adlittle, share2b, bandm, israel, bnl1)
   - Target after Phase 1: 10/12 pass
   - Target after all phases: 12/12 pass

3. **Specific problem tests**
   - beaconfd (140 equalities) - primary target
   - degen2 (degenerate) - cycling test
   - grow7 (scaling issues) - scaling test

## Success Criteria

| Metric | Current (Feb 2026) | Target |
|--------|---------|--------|
| NETLIB tiny suite | 3/5 pass | 5/5 pass |
| beaconfd | FAIL (singular at iter 163) | PASS |
| bnl1 | 0.009% objective error | <0.001% error |
| Unit tests | 76/76 pass | 76/76 pass |

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Two-phase slower than Big-M | Only use when needed; add heuristic |
| Scaling introduces new errors | Careful unscaling; test round-trip |
| Repair cycles | Track history; limit repair attempts |
| Regression on working problems | Comprehensive test suite |

## Open Questions

1. Should two-phase be default for all problems or only when Big-M fails?
2. What condition number threshold triggers proactive refresh?
3. Should we implement crash procedure for initial basis?
