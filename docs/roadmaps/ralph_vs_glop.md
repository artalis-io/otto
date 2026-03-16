# Ralph LP Solver: GLOP Comparison & Improvement Plan

## 2026-02-23 Addendum (API/Architecture Baseline `abd62fa`)

The original comparison focused on LP/MIP runtime behavior. Ralph now also has a significantly
improved API/architecture baseline:
- explicit LP/MIP solve entry points (`ralph_optimize_lp`, `ralph_optimize_mip`) with
  compatibility-only `ralph_optimize` dispatch,
- typed/scoped parameters with metadata/introspection,
- public LP algorithm capability/report contracts,
- dedicated LP backend dispatch module (`ralph/src/lp_dispatch.c`) with orthogonal tests,
- expanded LP diagnostics (presolve report, telemetry snapshots, solution quality, progress/cancel).

This means current parity discussion vs GLOP should treat API contract maturity separately from
remaining runtime/backend gaps. The major open LP backend gap remains real barrier/crossover
execution (API surface exists; capability is currently gated off with explicit fallback reporting).

## 2026-02-23 Addendum (External Adapter Contract Baseline)

LP external backend routing is now explicit-only:
- `primal`, `dual`, and `auto` stay on internal simplex by default.
- External routing requires an explicit external algorithm request and a matching
  `lp_external_provider` ID.
- If provider/backend is unavailable or mismatched, solver falls back to internal simplex with
  `RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE`.

**Date:** 2026-02-11
**Reference:** [Google OR-Tools GLOP](https://github.com/google/or-tools/tree/stable/ortools/glop)

## Overview

GLOP is Google's production LP solver inside OR-Tools. This document compares Ralph's LP
infrastructure against GLOP to identify the highest-impact improvements, particularly for
MIP branch-and-bound performance.

## Critical Gaps (Highest Impact)

### 1. Dual Simplex as Primary Algorithm

| Aspect | GLOP | Ralph (before) | Ralph (current) |
|--------|------|----------------|-----------------|
| Default for LP | Dual simplex | Primal simplex | Dual simplex (method=2 auto with primal fallback) |
| B&B re-optimization | Load parent basis, dual simplex does 1-5 pivots | `has_fixed_basic` check aborted warm start on ALL binary MIPs → cold start every node | `dual_simplex_solve_v2()` warm-start: update bounds, clear perturbation, recompute solution, dual pivots to restore primal feasibility |
| Dual steepest edge | Exact Forrest-Goldfarb DSE with incremental updates | Most-infeasible leaving variable, basic Harris | Exact DSE with incremental weight updates, persists across B&B nodes |
| Bound flipping | Flips boxed variable bounds without basis change | Every pivot does full LU update | Two-pass Harris bound flipping in dual ratio test |

**Status: IMPLEMENTED** (Phase E complete, `140a1f2`, Feb 2026)

Benchmarks (FuelWise MILP, seed=42):

| Scenario | Before (ms) | +dual_reopt | +HYBRID+PATH B | +presolve remap | +P5/P6 | +cut fix+pseu | Total Speedup | vs GLPK |
|----------|-------------|-------------|----------------|-----------------|--------|---------------|---------------|---------|
| milp15 | 2.28 | 1.07 | 0.99 | 0.95 | 0.85 | **1.29** | **1.8x** | **5.7x faster** |
| milp30 | 111.72 | 22.40 | 5.97 | 3.14 | 2.73 | **4.67** | **23.9x** | **1.8x faster** |
| milp50 | 197.36 | 53.39 | 19.09 | 9.90 | 9.28 | **27.76** | **7.1x** | 0.7x (1.5x slower) |
| milp75 | ~1232 | 573.14 | 151.89 | 55.49 | 33.26 | **63.34** | **19.5x** | ~tied |
| milp100 | ~7223 | 175.89 | 54.49 | 50.14 | 62.05 | **82.12** | **88.0x** | 0.3x (3x slower) |
| milp200 | ~19473 | 8987.79 | 890.40 | 212.03 | 994.96 | **780.10** | **25.0x** | 0.1x (9x slower) |

**"+cut fix+pseu" column** (seed 42): Cut generation normalization fix + pseudocost branching
+ root strong branching + node probing. Multi-seed results (5 seeds × 10 runs) in `ralph.md` §1.7.

**Key improvement: objective gap vs GLPK** (the "+cut fix" column trades speed for quality):

| Scenario | Gap Before (P5/P6) | Gap After (cut fix, seed 42) | Gap After (5-seed avg) |
|----------|--------------------|------------------------------|------------------------|
| milp15   | 13.97%             | **0.14%**                    | 25.7% (seed-dependent) |
| milp30   | 3.55%              | **1.83%**                    | 1.72% |
| milp50   | 0.39%              | 0.66%                        | 0.69% |
| milp75   | 0.72%              | **0.60%**                    | 0.55% |
| milp100  | 0.39%              | **0.21%**                    | 0.48% |
| milp200  | 9.23%              | **3.82%**                    | 0.87% |

**Improvements over original implementation:**
- **HYBRID node selection**: DFS until first incumbent, then best-bound.
- **Presolve with priority remapping**: Lightweight mask 0x110F. Branch priorities/directions
  remapped via `presolved->var_map`. Without remapping: 2-4x more nodes.
- **P5 bound flipping + P6 dual steepest edge**: In `dual_simplex_solve_v2`. Exact DSE
  with incremental weight updates persisting across B&B nodes.
- **Cut generation normalization fix** (`5b1bd4c`): Row normalization sign bug in GMI/c-MIR
  back-substitution. `tab->row_sign[con_row]` must be applied when accessing model coefficients
  during slack variable substitution. Without fix: cuts have inverted coefficients → invalid.
- **Safety guard for cut-induced infeasibility**: If LP becomes non-optimal after cuts, discard
  all cuts (rebuild from original model) and continue tree search.
- **Pseudocost branching + root strong branching**: Probe 20 fractional vars × 50 dual pivots.
  Obj-coeff init `fmax(|c_j|, 1.0)`, updated from actual bound changes.
- **Column-based probing**: Bound tightening at nodes with depth < 20.
- **Phase E (`140a1f2`)**: Replaced `dual_reopt` (3-path dispatch) with single warm-start
  path via `dual_simplex_solve_v2()`. Net -1124 LoC. MIP uses method=2 (auto: dual first,
  primal fallback) — same solver for LP and MIP.

**Remaining gap vs GLOP:** Ralph is now architecturally aligned with GLOP (dual simplex as
default for both LP and MIP). The main performance gap is supernodal LU factorization (T2.1)
for larger problems. GLOP also has dualization for constraint-heavy problems and more aggressive
probing (non-binary integer implications, clique detection).

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
| Triangular (GLPK-style) | Yes — singleton column priority | ✅ Yes (`crash_triangular`) |
| Maros LTSF | Yes — row/column priority scores | No |
| Default | Triangular crash | Triangular crash (primal), slack basis (dual) |

**Status: IMPLEMENTED** (`crash_triangular()` in `simplex.c`, primal simplex only)

### 4. Objective Limits for MIP Pruning

| Aspect | GLOP | Ralph |
|--------|------|-------|
| Early termination | `objective_lower_limit` / `objective_upper_limit` | ✅ `objective_limit` |
| MIP integration | LP terminates when bound exceeds incumbent | ✅ LP terminates when bound exceeds incumbent |

**Status: IMPLEMENTED** (`objective_limit` in `simplex.c`, used by B&B node solver)

## High Impact Gaps

### 5. DynamicMaximum Pricing (Top-K Heap)

**Status: IMPLEMENTED** (`pricing=4` in `ralph/src/simplex.c`, Feb 2026, commit `b3594c6`)

Binary max-heap over non-basic variables keyed by improvement score. Heap maintained incrementally
during `simplex_pivot()`. Critical bound-flip bug found and fixed (zombied heap entries from status
changes in bound-flip early-return path). NETLIB: 14/17 with heap pricing (matches Dantzig).

**Limitation:** Heap accelerates Dantzig's O(n) scan to O(1) extraction. NOT composable with
Devex or Steepest Edge — weighted scoring changes for ALL non-basic vars every pivot, making
heap maintenance O(n log n) which is worse than O(n) scan. Devex remains the default pricing
strategy. Heap pricing is available as `pricing=4` for Dantzig-style use cases.

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
Ralph: ✅ `verify_solution()` with Ax=b, bound, dual, complementary slackness, objective
checks. OPTIMAL→IMPRECISE downgrade. Kahan summation. Always-on for method=2.

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
| **P1** | Objective cutoff in dual | 30-50% fewer iters on pruned nodes | Low | P0 | **DONE** |
| **P2** | Crash basis (triangular) | 2-5x cold starts | Medium | None | **DONE** |
| **P3** | LP Presolve | 2-3x avg (41x best) | High | None | **DONE** |
| **P4** | DynamicMaximum pricing | 2-5x pricing | Low-Medium | None | **DONE** |
| **P5** | Bound flipping in dual | Fewer basis updates | Low | P0 | **DONE** |
| **P6** | Dual steepest edge | 2-3x fewer pivots | Medium | P0 | **DONE** |
| **P7** | Multi-pass scaling | Better numerics | Low | None | **DONE** |
| **P8** | Dual simplex as default LP + MIP | ~2x on initial solves | High | P2, P5, P6 | **DONE** (Phase D+E) |

## Completed Items

All priority items P0-P8 are now implemented. Key milestones:

### P8: Dual simplex as default LP + MIP — ✅ DONE

Completed in two phases:
- **Phase D** (`dac309a`): Changed default `method` from 0 to 2 (auto: dual first, primal fallback).
  `dual_simplex_solve_from_scratch_v2()` with proper `dual_phase1()`, exact DSE, bound
  perturbation with unshift cleanup, and `verify_solution()` safety net.
- **Phase E** (`140a1f2`): Replaced `dual_reopt()` in B&B with `dual_simplex_solve_v2()` warm-start.
  Collapsed `solve_node_lp()` from 3-path dispatch to single path. Deleted ~1124 net LoC including
  old `dual_simplex_solve()`, `dual_reopt()`, and associated helpers.

Ralph is now architecturally aligned with GLOP: dual simplex is the default for both
standalone LP and MIP node solving, with primal as automatic fallback.

### Remaining gap: Supernodal LU (T2.1)

The only remaining high-impact gap vs GLOP/CLP is supernodal LU factorization. Groups columns
with similar sparsity into dense blocks and uses BLAS-3 kernels. Would reduce LU from ~25% →
~10% of iteration time. ~1500 LoC effort, depends on T1.4 (done).

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
