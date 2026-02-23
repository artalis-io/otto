# Ralph vs Production LP/MIP Solvers — Frank Assessment

## 2026-02-23 Addendum (API/Architecture Baseline `abd62fa`)

This document started as a performance-only snapshot. Ralph now has a materially stronger LP API
contract and cleaner internal architecture than the original `f58b421` context used below.

What changed in API/architecture since that snapshot:
- Explicit LP/MIP solve entry points: `ralph_optimize_lp()` and `ralph_optimize_mip()`, with
  `ralph_optimize()` retained as compatibility dispatch only.
- Scoped and typed parameter surfaces (LP-only vs MIP-only) plus metadata/introspection APIs
  (`RalphParamId`, `ralph_get_param_meta`, `ralph_find_param_by_name`).
- Public LP algorithm capability/report surface:
  `ralph_get_lp_capabilities`, `ralph_get_last_lp_algorithm_report`,
  `RalphLPAlgorithm`, `RalphLPCrossoverMode`, `RalphLPFallbackReason`.
- Internal LP backend routing extracted into dedicated module:
  `ralph/src/lp_dispatch.c` + `ralph/src/lp_dispatch.h` (baseline `abd62fa`), with orthogonal
  module test coverage in `test_lp_dispatch`.
- LP diagnostics/operability expanded: presolve report, LP/LU telemetry snapshots, solution
  quality metrics, LP progress + cancellation hooks, typed LP conflict API.

Current gap framing vs GLOP/HiGHS should now be split into:
- Algorithmic/runtime performance gaps (still material on degenerate/outlier cases).
- Remaining backend gap: barrier/crossover execution backend is API-complete but still
  capability-gated off (`supports_barrier=0`, fallback semantics active).

Historical benchmark discussion below is retained as context, but API conclusions should be read
through the updated architecture above and the baseline in `docs/roadmaps/ralph.md`.

## 2026-02-23 Addendum (External Adapter Contract Baseline)

LP backend selection policy is now explicit and stricter:
- `primal`, `dual`, and `auto` algorithms are internal-first and do not auto-upgrade to external.
- External backends require explicit external algorithm selection
  (`*_EXTERNAL`) plus a matching `lp_external_provider` ID.
- Missing/mismatched/unavailable external provider now reports
  `RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE` and falls back to internal simplex.

This removes implicit external routing from the default LP path and makes external execution a
deliberate opt-in behavior.

**Date:** 2026-02-16
**Ralph version:** `f58b421` (universal two-phase + redundant row presolve)

## Summary

Ralph is a competent educational/embedded LP solver with all the standard textbook features
implemented. Calling it "92% of state-of-the-art" is accurate for **feature coverage** but
misleading for **performance**. On LP problems Ralph is 3-10x slower than GLPK and 50-200x
slower than GLOP/CLP. On MIP problems Ralph is 100-2000x slower than GLPK at scale (milp50+).

Ralph's strength is its zero-dependency design (compiles to WASM), rich feature set (LAP,
network flow, detection), and domain-specific MIP hints. It is well-suited for small-to-medium
problems (< 500 vars) where solve time is < 100ms regardless of solver efficiency.

---

## LP Solver: What's Done vs What Matters

### Feature Checklist (the "92%")

| Feature | Ralph | GLPK | CLP | GLOP | Notes |
|---------|-------|------|-----|------|-------|
| Revised simplex (primal) | Yes | Yes | Yes | Yes | |
| Dual simplex | Yes | Yes | Yes | Yes | Ralph's is new (Phase D/E) |
| LU factorization | Yes | Yes | Yes | Yes | |
| Sparse LU (Markowitz) | Yes | Yes | Yes | Yes | |
| Symbolic/numeric separation | Yes | Yes | Yes | Yes | T1.4 done |
| Devex pricing | Yes | Yes | Yes | Yes | |
| Steepest edge (primal) | Partial | Yes | Yes | Yes | Ralph: Devex approximation only |
| Dual steepest edge | Yes | Yes | Yes | Yes | P6 |
| Bound perturbation | Yes | Yes | Yes | Yes | Primal + dual |
| Harris ratio test | Yes | Yes | Yes | Yes | |
| Bound flipping (dual) | Yes | Yes | Yes | Yes | P5 |
| Scaling (geometric + equil) | Yes | Yes | Yes | Yes | T1.2 |
| Crash basis | Yes | Yes | Yes | Yes | T1.1 (primal only) |
| Presolve | Yes | Yes | Yes | Yes | 12 techniques |
| Objective limits | Yes | Yes | Yes | Yes | T3.1 |
| Post-solve verification | Yes | No | Yes | Yes | T2.3 |
| Hyper-sparse FTRAN/BTRAN | Yes | No | Yes | Yes | DFS-based |
| Dynamic refactorization | Yes | Yes | Yes | Yes | T3.2 |
| Two-phase simplex (no Big-M) | Yes | No | Yes | Yes | Universal since `f58b421` |
| Redundant row presolve | Yes | No | Yes | Yes | Equality-only rank detection |
| NETLIB pass rate | 20/22 | 17/17 | 17/17 | 17/17 | Ralph: beaconfd, lotfi fail |

### What the Checklist Doesn't Show

**1. LU factorization quality (the dominant cost)**

LU factorization is 42% of Ralph's per-iteration time, and Ralph's LU is 5-10x slower than
GLPK's `bflib` (which uses supernodal techniques) and 20-50x slower than CLP's `CoinFactorization`
(which uses dense BLAS for supernodes + sophisticated pivot ordering).

Ralph has sparse Markowitz LU with threshold pivoting. This is correct but slow. The symbolic/
numeric separation (T1.4) helps with refactorization reuse, but the numeric phase itself is
still column-by-column scalar operations where production solvers use vectorized dense blocks.

**Missing: T2.1 Supernodal LU** (~1500 LoC). This is the single biggest LP performance gap.
Without it, Ralph will always be 3-5x slower per iteration on m > 200.

**2. FTRAN/BTRAN spike handling**

Ralph uses an FT (Forrest-Tomlin) update scheme with a contiguous spike pool. This is good
design, but the forward/backward solve through spikes is still scalar. Production solvers
batch spike applications and use BLAS-2 (matrix-vector) operations.

**3. Steepest Edge pricing quality**

Ralph has Devex (approximate steepest edge) for primal and exact DSE for dual. CLP and GLOP
use exact steepest edge for primal too, with sophisticated weight maintenance. Devex typically
needs 10-30% more iterations than exact SE on degenerate problems.

**4. Presolve depth**

Ralph: 12 techniques. GLOP: 20+ techniques including probing with implication graphs,
substitution chains, and coefficient strengthening. CLP: similar depth. More aggressive
presolve means smaller problems reach the simplex, which compounds the LU speed difference.

**5. Numerical robustness on hard problems**

NETLIB is a small suite. Ralph passes 20/22 fast-tier problems (beaconfd fails due to 57
stuck artificials in Phase 1→2 transition creating near-singular basis; lotfi fails due to
LU instability producing spurious unboundedness). Universal two-phase simplex (no Big-M)
fixed kb2/recipe/scorpion objective errors but exposed Phase 1→2 transition fragility.
The Mittelmann benchmarks (200+ problems, many ill-conditioned) would expose more gaps.
Production solvers have decades of special-case handling for near-singular bases, cycling
detection, refactorization strategies, and recovery paths.

### Honest LP Performance Assessment

| Problem size | Ralph vs GLPK | Ralph vs CLP/GLOP | Notes |
|-------------|---------------|-------------------|-------|
| < 100 vars | 1-3x slower | 5-10x slower | Overhead dominates |
| 100-500 vars | 3-5x slower | 10-30x slower | LU gap grows |
| 500-2000 vars | 5-10x slower | 30-100x slower | Supernodal gap |
| > 2000 vars | 10-20x slower | 100-500x slower | Dense BLAS gap |

Ralph is competitive only on tiny problems where total solve time is < 10ms regardless.

---

## MIP Solver: The Real Gap

The MIP solver gap is much larger than the LP gap because MIP amplifies LP weaknesses
(hundreds of node LPs) and adds its own algorithmic gaps.

### Current MIP Benchmarks (FuelWise, seed 42)

| Scenario | Ralph | GLPK | Ratio | Obj gap |
|----------|-------|------|-------|---------|
| milp15 | 20 ms | 7.8 ms | 2.6x | 124.5% |
| milp30 | 176 ms | 9 ms | 20x | 0.21% |
| milp50 | 1.6 s | 13 ms | 122x | 36.5% |
| milp75 | 5.5 s | 59 ms | 93x | 0.20% |
| milp100 | 39 s | 18 ms | 2170x | 7.2% |
| milp200 | 224 s | 91 ms | 2450x | 5.9% |

### Why Ralph's MIP Is Slow

**1. Per-node LP cost is too high**

The dominant issue. Each B&B node solves an LP via `dual_simplex_solve_v2()` warm start.
This involves:
- Bound perturbation: O(n) — every node
- DSE weight validation: O(1) amortized (conditional init), but O(m²) when invalidated
- Dual Phase 2 pivots: typically 5-50 iterations per node
- Bound unshift + cleanup pivots: O(n) + a few pivots — every node

GLPK's node solver is lighter: it reuses the basis directly, applies bound changes incrementally,
and does dual pivots without the overhead of perturbation setup/teardown per node.

The old `dual_reopt` was purpose-built for this: approximate DSE (weights=1.0), no perturbation
(budget-limited), no unshift. It was 10-100x faster per node but less numerically precise.

**2. Cut generation is weak**

Ralph: GMI + c-MIR (2 families). GLPK: 7+ families (cover, clique, flow cover, MIR, Gomory,
implicit bounds, GUB covers). FuelWise MILPs have knapsack structure → cover cuts would help
enormously. This explains the objective gap on milp15/milp50 where the LP relaxation is loose.

**3. No cut pool management**

Ralph keeps all generated cuts in the working LP forever. At milp100+, the LP grows to 3-5x
its original size, making each node solve 3-5x slower. GLPK prunes inactive cuts aggressively.

**4. Limited heuristics**

Ralph: diving + RINS. GLPK: RINS + feasibility pump + LP-guided rounding + polishing.
More heuristic diversity finds better incumbents faster, which improves pruning.

**5. No conflict analysis**

When a node is infeasible, Ralph doesn't learn from the conflict. GLPK/SCIP add no-good
constraints that prevent re-exploring the same branching combination.

### Pre-Phase-E vs Post-Phase-E

Phase E was a code quality improvement (replace 3-path dispatch with single clean path,
delete 1124 lines) but a performance regression for MIP. The old `dual_reopt` was ugly but
fast. Options going forward:

| Option | Pros | Cons |
|--------|------|------|
| Revert Phase E | Restore pre-E MIP speed | Reintroduce 1124 lines of complex code |
| Optimize v2 for warm-start | Keep clean code, recover speed | Needs careful work (skip perturb/unshift when budget-limited) |
| Accept tradeoff | Simplest | milp50+ unusable vs GLPK |
| **Recommended: lightweight warm-start mode for v2** | Best of both | Medium effort (~200 LoC) |

The recommended path: add a "lightweight" mode to `dual_simplex_solve_v2` that skips
perturbation and unshift when called with a budget < 500 pivots. This recovers most of
`dual_reopt`'s speed while keeping the clean single-path architecture.

---

## What "State-of-the-Art" Actually Means

### The LP SoTA Stack (2026)

| Tier | Solvers | Characteristics |
|------|---------|----------------|
| **Tier 1: Commercial** | Gurobi, CPLEX, Xpress | 100-1000x faster than Ralph on large LP. Parallel simplex, barrier method, crossover. Decades of engineering. |
| **Tier 2: Production open-source** | HiGHS, CLP/CBC, GLOP | 30-200x faster than Ralph. Supernodal LU, exact SE, sophisticated presolve, parallel. |
| **Tier 3: Good open-source** | GLPK | 3-20x faster than Ralph. Similar feature set, more mature. |
| **Tier 4: Embedded/educational** | **Ralph**, lp_solve, COIN-OR Vol | Correct results, adequate for small problems, limited scalability. |

Ralph is solidly **Tier 4** — a well-featured embedded solver. The "92% feature coverage"
means it has 92% of the algorithmic building blocks, but many are implemented at 30-50% of
production quality (especially LU factorization, which dominates runtime).

### What's Missing to Reach Tier 3 (match GLPK)

| Gap | Effort | Impact | Priority |
|-----|--------|--------|----------|
| **Phase 1→2 transition robustness** | ~100 LoC | Fix beaconfd/lotfi (22/22 NETLIB) | Critical |
| **Supernodal LU** | ~1500 LoC | 3-5x LP speedup | Critical |
| **Lightweight MIP warm-start** | ~200 LoC | 10-100x MIP speedup | Critical |
| **Cover cuts** | ~400 LoC | Fix milp15/milp50 obj gap | High |
| **Cut pool management** | ~300 LoC | 3-5x milp100+ speed | High |
| **Feasibility pump** | ~300 LoC | Better incumbents | Medium |
| Exact primal SE | ~200 LoC | 10-30% fewer iterations | Medium |

**Total: ~3000 LoC to match GLPK quality.**

### What's Missing to Reach Tier 2 (match HiGHS/CLP)

Everything from Tier 3, plus:

| Gap | Effort | Impact |
|-----|--------|--------|
| Dense BLAS kernels (no external dep) | ~2000 LoC | 3-5x LU on dense submatrices |
| Interior point method (barrier) | ~3000 LoC | 10-100x on large sparse LP |
| Crossover (barrier → simplex basis) | ~500 LoC | Required for barrier |
| Parallel simplex (OpenMP) | ~1000 LoC | 2-4x on multi-core |
| Advanced presolve (substitution chains) | ~1000 LoC | Smaller problems |
| Conflict analysis for MIP | ~600 LoC | 20-40% fewer MIP nodes |
| Node-level cuts | ~200 LoC | Tighter bounds in tree |
| Multiple cut families (5+) | ~2000 LoC | Better LP relaxations |

**Total: ~12000+ additional LoC to match HiGHS.** This is roughly 6 months of focused work.

### What's Missing to Reach Tier 1 (match Gurobi)

Not achievable as an open-source project. Tier 1 solvers have:
- 500K+ lines of solver code
- Decades of special-case numerical handling
- Proprietary parallel algorithms
- Hardware-specific tuning (AVX-512, GPU)
- Dedicated teams of 10-30 optimization PhDs

---

## Recommendation

Ralph should **not** try to compete with production LP/MIP solvers on raw performance. Instead:

1. **Fix the critical MIP gaps** (lightweight warm-start, cover cuts, cut pool) — ~2400 LoC,
   achievable in 1-2 weeks. This makes milp100 usable and closes the objective gap.

2. **Keep the unique strengths**: zero-dependency WASM compilation, LAP/network flow detection,
   domain-specific MIP hints, transport-agnostic design.

3. **For production MIP at scale**: integrate HiGHS as an optional backend
   (`ralph_set_backend(RALPH_BACKEND_HIGHS)`). Let Ralph handle detection, model building,
   and domain hints; let HiGHS handle the heavy solving.

4. **Supernodal LU** (T2.1) is worth doing for LP performance, but it's a large effort with
   diminishing returns given that MIP warm-start is the actual bottleneck for FuelWise.

The honest answer: Ralph is a good solver for problems with < 200 integer variables and
< 500 total constraints. Beyond that, use a production solver.
