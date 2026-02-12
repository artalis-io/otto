# Ralph LP Solver: GLOP Comparison & Improvement Plan

**Date:** 2026-02-11
**Reference:** [Google OR-Tools GLOP](https://github.com/google/or-tools/tree/stable/ortools/glop)

## Overview

GLOP is Google's production LP solver inside OR-Tools. This document compares Ralph's LP
infrastructure against GLOP to identify the highest-impact improvements, particularly for
MIP branch-and-bound performance.

## Critical Gaps (Highest Impact)

### 1. Dual Simplex as Primary Algorithm

| Aspect | GLOP | Ralph (before) | Ralph (after `dual_reopt`) |
|--------|------|----------------|---------------------------|
| Default for LP | Dual simplex | Primal simplex | Primal (cold start), Dual (B&B reopt) |
| B&B re-optimization | Load parent basis, dual simplex does 1-5 pivots | `has_fixed_basic` check aborted warm start on ALL binary MIPs → cold start every node | `dual_reopt()`: 1-3 pivots avg, PATH A (direct child, skip refactorize) / PATH B (non-child, reuse LU + larger budget) / PATH C (cold start fallback) |
| Dual steepest edge | Exact Forrest-Goldfarb DSE with incremental updates | Most-infeasible leaving variable, basic Harris | Same as before (most-infeasible) |
| Bound flipping | Flips boxed variable bounds without basis change | Every pivot does full LU update | Same as before |

**Status: IMPLEMENTED** (`dual_reopt` in `ralph/src/dual_simplex.c`, Feb 2026)

Benchmarks (FuelWise MILP, seed=42):

| Scenario | Before (ms) | After dual_reopt (ms) | After HYBRID+PATH B (ms) | Total Speedup | vs GLPK |
|----------|-------------|----------------------|--------------------------|---------------|---------|
| milp15 | 2.28 | 1.07 | **0.99** | **2.3x** | **9.0x faster** |
| milp30 | 111.72 | 22.40 | **5.97** | **18.7x** | **1.9x faster** |
| milp50 | 197.36 | 53.39 | **19.09** | **10.3x** | 0.7x (1.5x slower) |
| milp75 | ~1232 | 573.14 | **151.89** | **8.1x** | 0.2x (6.3x slower) |
| milp100 | ~7223 | 175.89 | **54.49** | **132.6x** | 0.3x (3.4x slower) |
| milp200 | ~19473 | 8987.79 | **890.40** | **21.9x** | 0.1x (7.9x slower) |

**Improvements beyond initial dual_reopt:**
- **HYBRID node selection** (`a3cd864`): DFS until first incumbent, then best-bound.
  `NodeQueue.has_incumbent` flag with one-time O(n) heap rebuild.
- **PATH B LU reuse**: Profiling milp50 showed 90% of time in `lu_factorize_dense` from
  PATH B's `restore_basis_from_node()` → `tableau_refactorize()` (O(m³)). Fix: skip basis
  restore, reuse current LU factors, update bounds, run `dual_reopt` with larger budget
  (10×m, cap 2000). Each pivot O(m) vs O(m³) refactorize. If budget exceeded, fall to PATH C.

**Bug found during implementation:** Degenerate artificial variables (basic at value 0 after
Phase 2) become non-zero when bounds change, corrupting the objective with BIG_M terms.
Fix: `dual_reopt` checks for basic artificials and bails to cold start if found.

**Remaining gap vs GLOP:** Ralph still uses primal simplex for initial LP solves and cold
starts. GLOP uses dual simplex as the *default* LP algorithm — not just for B&B reopt.
This matters because dual simplex benefits from crash basis (no Phase 1 needed) and steepest
edge pricing. Ralph's `dual_simplex_solve()` is too heavy for this role — it has 5 internal
fallback paths to primal simplex, bound perturbation, and complex stalling detection. A
production dual-as-default would need: crash basis (P2), dual steepest edge (P6), bound
flipping (P5), and a cleaner `dual_simplex_solve()` without primal fallbacks.

### 2. Presolve (15+ Preprocessors)

| Aspect | GLOP | Ralph |
|--------|------|-------|
| Fixed variables | Yes (lb == ub elimination) | ✅ Yes |
| Singleton rows/columns | Yes (tighten bounds, fix vars) | ✅ Yes (bound tightening + deletion) |
| Forcing constraints | Yes (all vars at bounds) | ✅ Yes |
| Implied free | Yes (bounds implied by constraints) | ✅ Yes (tightens to finite implied bounds) |
| Doubleton equality | Yes (substitution elimination) | ✅ Yes (with postsolve stack) |
| Proportional columns/rows | Yes (dominated column detection) | ✅ Yes (rows and columns) |
| Shift variable bounds | Yes | ✅ Yes (with postsolve SHIFT ops) |
| MIP probing | Yes (implication propagation) | ✅ Yes (probing + bound tightening reuse) |
| Dualization | Yes (auto-dual when constraints >> vars) | No |
| Presolve loop | Up to 20 passes until fixed-point | ✅ Up to 20 passes |
| Redundant row detection | Implicit via reductions | ✅ Explicit Gaussian elimination |
| Empty row/col removal | Yes | ✅ Yes |
| Bound tightening | Yes | ✅ Yes (with cancellation guards) |
| Shared row bounds primitive | Implicit | ✅ `compute_row_bounds()` used by forcing, tightening, probing |

**Status: IMPLEMENTED** (P3 in `ralph/src/presolve.c`, Feb 2026)

Benchmarks (FuelWise MILP, seed=42, post-P3 vs pre-P3):

| Scenario | Before (ms) | After (ms) | Speedup |
|----------|-------------|------------|---------|
| milp15 (10 runs) | 1.72 | 1.07 | **1.6x** |
| milp30 (5 runs) | 65.50 | 22.21 | **2.9x** |
| milp50 (3 runs) | 124.14 | 52.68 | **2.4x** |
| milp75 (3 runs) | 1231.65 | 573.14 | **2.1x** |
| milp100 (3 runs) | 7223.45 | 175.89 | **41.1x** |
| milp200 (3 runs) | 19472.76 | 8987.79 | **2.2x** |

**Key implementation details:**
- Multi-round fixed-point loop (20 iterations, matching GLOP)
- Singleton row bound tightening for LE/GE/EQ constraints
- Doubleton equality elimination with CSC fill-in check, integer variable guard
- Implied free variable detection (tightens to finite bounds, not ±infinity, to avoid
  breaking Big-M in simplex Phase 1)
- Proportional row/column detection (rows: redundancy/tighter dominance; columns: cost-dominated fixing)
- Shift-variable-bounds with `POSTSOLVE_SHIFT` ops (skips integer/binary variables)
- MIP probing with implication propagation: orthogonally reuses `presolve_bound_tightening()`
  rather than duplicating constraint-based bound propagation
- `compute_row_bounds()` shared primitive eliminates duplication between forcing constraints
  and bound tightening (all callers get cancellation guards and finiteness tracking)
- LIFO postsolve stack (`PostsolveOp`) for recovering original variable values

**Remaining gap vs GLOP:** Dualization for constraint-heavy problems. Ralph also lacks
GLOP's more aggressive probing (non-binary integer implications, clique detection).

### 3. Crash Basis (Advanced Initial Basis)

| Aspect | GLOP | Ralph |
|--------|------|-------|
| Bixby (1992) | Yes — near-triangular basis | No |
| Triangular (GLPK-style) | Yes — singleton column priority | No |
| Maros LTSF | Yes — row/column priority scores | No |
| Default | Triangular crash | All-slack/artificial basis |

**Why it matters:** Crash eliminates Phase 1 entirely on many problems. Ralph always does
Phase 1 → Phase 2 (or Big-M), adding 30-50% overhead.

**Expected impact:** 2-5x on cold starts.

### 4. Objective Limits for MIP Pruning

| Aspect | GLOP | Ralph |
|--------|------|-------|
| Early termination | `objective_lower_limit` / `objective_upper_limit` | None |
| MIP integration | LP terminates when bound exceeds incumbent | LP solves to full optimality |

**Expected impact:** 30-50% fewer LP iterations in later B&B nodes.

## High Impact Gaps

### 5. DynamicMaximum Pricing (Top-K Heap)

GLOP maintains a top-32 heap of best pricing candidates. Serves 98% of queries from cache,
reducing pricing overhead from ~60% to ~3% of total time.

Ralph scans all non-basic variables every iteration (O(n)). Partial pricing helps but
is less sophisticated (round-robin scan of 100 variables).

**Expected impact:** 2-5x on larger problems (100+ variables).

### 6. Bound Flipping in Dual Ratio Test

GLOP's dual ratio test flips boxed variable bounds without a basis change, avoiding
expensive LU updates. Critical for problems with many bounded variables.

Ralph does a full basis update for every dual pivot.

**Expected impact:** Significant on FuelWise MILPs (many bounded binary variables).

### 7. Dual Steepest Edge Pricing

GLOP: Exact DSE norms (Forrest & Goldfarb 1992, Koberstein 2005) with incremental updates,
precision monitoring, automatic full recomputation when drift exceeds threshold.

Ralph: Dual simplex uses most-infeasible leaving variable with basic Harris tie-breaking.
No edge norm maintenance.

**Expected impact:** 2-3x fewer dual pivots.

### 8. Multi-Pass Equilibrium Scaling

GLOP: Iterative equilibrium scaling until convergence, plus 4 cost scaling methods
(contain-one, mean, median, none).

Ralph: Single-pass geometric mean scaling.

**Expected impact:** Better numerical behavior, fewer degenerate pivots.

## Medium Impact Gaps

### 9. Middle Product Form LU Updates

GLOP uses rank-one matrices (`I + u*v^T`) — more numerically stable than Forrest-Tomlin,
better sparsity exploitation. Ralph uses FT with spike compaction to dense matrix every
300 updates (O(m^2), doesn't scale past m ~5000).

### 10. Dynamic Refactorization Period

GLOP: Default 64 iterations, dynamically adjusted based on measured solve overhead.
Ralph: Fixed thresholds (50-200), growth-triggered.

### 11. Solution Verification

GLOP: 7 independent post-solve metrics (primal/dual infeasibility, reduced cost, activity,
objective error), status downgrade to IMPRECISE. Kahan summation for objective.
Ralph: No post-solve verification.

### 12. Re-optimization Detection

GLOP: Detects when only bounds changed between solves, skips unnecessary work (no new
tableau creation). Ralph's `simplex_solve()` creates a new tableau from scratch every time.

## What Ralph Does Well

- **Hyper-sparse FTRAN/BTRAN**: DFS-based reach computation, density-adaptive switching
- **Contiguous spike pool**: Cache-friendly FT storage with offset indexing
- **Phase-1 recovery**: Dual rescue, entering exclusion, redundant row marking
- **Arena allocator**: Minimal allocation overhead in hot paths
- **LP Presolve**: 12 techniques with orthogonal design — shared `compute_row_bounds()` primitive,
  probing reuses `presolve_bound_tightening()`, 105 unit tests
- **For target problem sizes** (100s-low 1000s of variables): capable and fast

## Implementation Priority

| Priority | Improvement | Impact | Effort | Dependencies | Status |
|----------|-------------|--------|--------|-------------|--------|
| **P0** | Dual simplex for B&B reopt | 2-5x MIP solves | Medium | None | **DONE** |
| **P1** | Objective cutoff in `dual_reopt` | 30-50% fewer iters on pruned nodes | Low | P0 | Stub exists |
| **P2** | Crash basis (triangular) | 2-5x cold starts | Medium | None | |
| **P3** | LP Presolve | 2-3x avg (41x best) | High | None | **DONE** |
| **P4** | DynamicMaximum pricing | 2-5x pricing | Low-Medium | None | |
| **P5** | Bound flipping in dual | Fewer basis updates | Low | P0 | |
| **P6** | Dual steepest edge | 2-3x fewer pivots | Medium | P0 | |
| **P7** | Multi-pass scaling | Better numerics | Low | None | |
| **P8** | Dual simplex as default LP | ~2x on initial solves | High | P2, P5, P6 | |

## Still TODO (from dual_reopt implementation)

### P1: Objective cutoff in `dual_reopt`

The `objective_cutoff` field exists in `SimplexSolver` and is set to `solver->best_obj *
obj_sense` before each `dual_reopt` call. The cutoff check itself is commented out because
`tab->obj_value` is only accurate after `tableau_compute_solution()` — it's NOT updated
after each `dual_simplex_pivot()`.

**Fix options:**
1. Call `tableau_compute_solution()` every N pivots (cheap but not every pivot)
2. Track objective delta during pivot: `delta_obj = theta * reduced_cost[entering]`
3. Maintain a running objective by updating `tab->obj_value += delta` after each pivot

Option 3 is what GLOP does. Requires verifying the delta formula for dual pivots:
`delta_obj = theta_dual * (x_leaving - bound_leaving)`. Low effort, moderate impact on
large B&B trees where many nodes are pruned by bound.

### P5: Bound flipping in dual ratio test

During `dual_ratio_test`, when a boxed variable hits its opposite bound, GLOP flips the
bound without a basis change (no LU update). This is especially valuable for FuelWise MILPs
where binary variables are boxed [0,1]. Currently every dual pivot does a full LU update
via `dual_simplex_pivot()`.

### P6: Dual steepest edge pricing

`dual_reopt` currently uses most-infeasible leaving variable selection. DSE (Forrest &
Goldfarb 1992) maintains edge norms incrementally and selects the leaving variable that
gives the steepest descent. Typically reduces pivot count by 2-3x. Requires:
1. Initialize DSE norms from current basis
2. Update norms after each pivot (one BTRAN + inner products)
3. Monitor precision and full-recompute when drift exceeds threshold

### P8: Dual simplex as default LP algorithm

**Why we're NOT doing this yet:** Ralph's `dual_simplex_solve()` is designed as a
re-optimization tool, not a standalone LP solver. It has:
- 5 internal fallback paths to `simplex_solve()` (primal cold start)
- Bound perturbation machinery (unnecessary for short reopt, essential for default)
- Complex stalling/cycling detection with 50-iteration thresholds
- No crash basis support (starts from whatever basis the tableau has)

GLOP uses dual simplex as default because it has:
- Crash basis that provides a dual-feasible starting point (no Phase 1)
- Steepest edge pricing that avoids degenerate cycling
- Bound flipping that handles boxed variables efficiently
- Clean separation: dual simplex IS the solver, not a helper

**Path to dual-as-default:**
1. Implement crash basis (P2) — eliminates Phase 1 for dual simplex
2. Implement bound flipping (P5) — makes dual efficient on bounded problems
3. Implement DSE (P6) — avoids degenerate cycling without perturbation
4. Rewrite `dual_simplex_solve()` as a standalone solver without primal fallbacks
5. Make it the default, keep primal as fallback for edge cases

This is a significant architectural change. `dual_reopt` was the easy win — it handles the
B&B case (95% of MIP time) with a focused 130-line function. Making dual the default LP
algorithm is a different scale of work.

## Presolve Quality: Ralph vs GLPK/GLOP

### Current State

Ralph tracks presolve stats (`vars_removed`, `cons_removed`, `bounds_tightened`) but has
**no public getter API** — stats are only visible via `verbose=1` printf output in
`ralph_optimize()`. The `PresolveResult` is created and consumed internally.

### Comparison Infrastructure

Existing GLPK comparison tools (`compare_glpk.c`, `bench_vs_glpk.c`, `bench_mip.c`) exist
but **don't capture presolve stats from GLPK**. They only compare solve time, iteration
count, and objective match.

### Gaps to Fill

| Gap | Impact | Effort |
|-----|--------|--------|
| Public presolve stats API (`ralph_get_presolve_stats()`) | Enables programmatic comparison | Low |
| `ralph_write_mps` implementation (declared but unimplemented) | Blocks easy export for side-by-side comparison | Medium |
| Benchmark tools capturing reduction ratios from both solvers | Enables direct presolve quality comparison | Medium |

### Known Numbers

**beaconfd (NETLIB):**
- Ralph presolve: 262 vars → 148 vars (43.5%), 173 cons → 87 cons (49.7%)
- Result: `OPTIMAL` in 149 iterations (was `ITERATION_LIMIT` without presolve)

## References

- Forrest & Goldfarb (1992) — Steepest edge for dual simplex
- Koberstein (2005) — Dual simplex, degeneracy, cost perturbation
- Huangfu Q (2013) — High-performance simplex solver
- Bixby (1992) — Initial basis heuristic
- Harris (1973) — Devex pricing
- Maros — Revised simplex, LTSF crash
