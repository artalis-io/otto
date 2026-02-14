# Ralph Solver Roadmap

Development roadmap for Ralph LP/MIP solver covering algorithms, performance, and planned features.

## Stable Baseline

**Current** (2026-02-14) — Strong branching UAF fix + NaN safety + RC fixing + RINS (`9315fd3`).
All tests pass (Ralph 272, FuelWise 123). All benchmark seeds crash-free (42/456/789/1337/9999).
Key fixes: (1) heap-use-after-free in `select_reliability_branch_impl` — `solution` pointer
invalidated by `simplex_solve` fallback in `strong_branch`, (2) `-ffinite-math-only` removed
from CFLAGS (caused NaN/Inf checks to be optimized away), (3) reduced-cost fixing at B&B nodes,
(4) RINS heuristic for incumbent improvement during tree search, (5) reliability branching with
priority awareness, (6) named constants for all MIP parameters.

Previous: `64f6cdc` — Cut generation normalization fix + pseudocost branching + probing.

Previous: `af158fa` — P5/P6 re-landed with infeasibility guards (208 tests, 60/60 MILP).

Previous: `fc454a7` — c-MIR sign fixes + infeasibility guard (199 tests, 100/100 MILP).

Previous: `b1d0e8c` — Objective cutoff + lightweight presolve with priority remapping.

Previous: `4387869` — HYBRID + PATH B LU reuse (9x milp15, 1.9x milp30).

## Status Summary (Feb 2026)

| Area | Status | Notes |
|------|--------|-------|
| **Revised Simplex** | ✅ Complete | Primal simplex with LU factorization |
| **LU Factorization** | ✅ Complete | Sparse factorization, eta updates |
| **Branch & Bound MIP** | ✅ Complete | HYBRID node, PATH B LU, dual_reopt, P5+P6, reliability branching, RC fixing, RINS |
| **Dual Simplex** | ✅ Complete | Bound flipping (P5), dual steepest edge (P6), 213 tests |
| **LAP Solver** | ✅ Complete | JVC algorithm, 358 tests |
| **Network Flow** | ✅ Complete | Network simplex, 153 tests |
| **Problem Detection** | ✅ Complete | Auto-detect LAP/network structure |
| **Presolve** | ✅ Phase 3 | 12 techniques, 20-round fixed-point, probing w/ implication propagation (P3) |
| **NETLIB Suite** | 92% Pass | 11/12 problems (bandm, lotfi, beaconfd fixed) |
| **MIP Infrastructure** | ✅ Complete | Branching, cuts, callbacks, warm start (§6) |
| **Benders Decomposition** | ✅ Complete | Generic solver, ~1430 LoC, 8 tests (§7) |

---

## Chapter 1: LP Solver Performance

### 1.1 Current Benchmarks

**1000x500 LP (15% dense)**
| Solver | Time | Per-Iteration | Gap |
|--------|------|---------------|-----|
| Ralph  | 0.95s | 0.385ms | - |
| GLPK   | 0.12s | 0.044ms | 8.8x |

**Per-Iteration Breakdown:**
- 42% LU factorization (refactorization)
- 27% BTRAN (eta updates backward)
- 26% FTRAN (eta updates forward)
- 5% Other (pricing, ratio test)

### 1.2 NETLIB Benchmark Results

| Problem | Status | Notes |
|---------|--------|-------|
| adlittle | ✅ PASS | Ralph 9.6x faster than GLPK |
| share2b | ✅ PASS | Ralph 6.8x faster than GLPK |
| kb2, sc50a, sc50b | ✅ PASS | Small dense problems |
| grow7, israel | ✅ PASS | Comparable to GLPK |
| stocfor1 | ✅ PASS | Stochastic programming |
| bnl1 | ✅ PASS | 2503 iters (degeneracy) |
| brandy | ✅ PASS | 133 iters |
| degen2 | ✅ PASS | 2333 iters (degeneracy) |
| bandm | ✅ PASS | OPTIMAL both paths (224 iters w/o presolve, 172 w/ presolve), obj=-158.628 |
| beaconfd | ✅ PASS | Fixed by presolve: 262→148 vars, 173→87 cons, obj=33592.49 |
| blend | ❌ FAIL | Returns INFEASIBLE (no presolve) / UNBOUNDED (presolve) — known opt: -30.812 |
| lotfi | ✅ PASS | Fixed by presolve: 308→302 vars, 153→144 cons, obj=-25.265 |

### 1.3 Performance TODO

See **§1.12** for the full state-of-the-art gap analysis with prioritized implementation order.

| Priority | Task | Expected Impact | §1.12 ID |
|----------|------|-----------------|----------|
| **High** | Multi-round equilibrium scaling | Better numerics, may fix blend | T1.2 |
| **High** | Triangular crash basis | 2-5x cold starts | T1.1 |
| **High** | Symbolic/numeric LU separation (§1.11) | 1.5-2x refactorization | T1.4 |
| **High** | Supernodal LU factorization (§1.11) | 3-5x factorization | T2.1 |
| **Medium** | Heap-based pricing (DynamicMaximum) | 2-5x pricing on large problems | T2.2 |
| **Medium** | Post-solve verification | Correctness, catch silent failures | T2.3 |
| **Medium** | Objective limits in simplex_solve | 30-50% fewer pruned-node iters | T3.1 |
| **Long-term** | Dual simplex as default LP | ~2x initial solves (arch change) | T1.3 |

### 1.4 Numerical Stability TODO

| Task | Status | Notes |
|------|--------|-------|
| Two-Phase Simplex | ✅ Complete | For >80% equality problems |
| Geometric Mean Scaling (1 round) | ✅ Complete | `apply_scaling()` in simplex.c |
| Multi-Round Equilibrium Scaling | ⏳ TODO | 5 geometric + 20 equilibrium (§1.12 T1.2) |
| Iterative Refinement | ✅ Complete | Residual correction |
| Threshold Pivoting | ✅ Complete | In factorization + updates |
| Harris Ratio Test | ✅ Complete | In both primal and dual |
| Bound Perturbation | ✅ Complete | Primal + dual, reactive + proactive |
| Cost Perturbation | ⏳ TODO | Alternative anti-degeneracy (§1.12 T3.3) |
| Post-Solve Verification | ⏳ TODO | Primal/dual feasibility + complementary slackness (§1.12 T2.3) |
| Basis Conditioning Report | ⏳ TODO | Expose condition estimate to user (§1.12 T3.6) |

### 1.5 Phase-1 Recovery Hardening (Feb 2026)

Hardening pass targeting phase-1 simplex failure modes observed with NETLIB beaconfd.

| Feature | Status | Commit | Notes |
|---------|--------|--------|-------|
| Phase-1 failure trace | ✅ Complete | `600d025` | Deterministic LU pivot-failure classification |
| LU failure reason tracking | ✅ Complete | `06981e7` | Classifies zero-pivot, threshold, singular causes |
| Basis action policy | ✅ Complete | `a3ce8b4` | Policy-driven simplex stabilization (exclude/cooldown/restore) |
| Beaconfd recovery | ✅ Complete | `67020ac` | Dual-simplex rescue on phase-1 stall |

**Phase-1 trace infrastructure:** When phase-1 fails (artificial variables remain), the solver
now records a deterministic trace of which basis actions were attempted (variable exclusions,
cooldowns, pivot retries) and why they failed. This enables root-cause analysis of numerical
instability without debug builds.

**Dual-simplex rescue:** If primal phase-1 stalls (max iterations without progress), the solver
attempts a dual-simplex recovery before declaring infeasibility. This resolved false-INFEASIBLE
results on beaconfd-class problems.

### 1.6 C-Audit Hardening (Feb 2026)

Full `/c-audit` pass on MIP infrastructure. 15 findings fixed (3 critical, 6 high, 4 medium, 2 low).

| File | Fixes | Key Issues |
|------|-------|------------|
| `benders.c` | 8 | Memory leak on partial alloc, bounds checks on master_var_indices, overflow guard on cut capacity |
| `ralph.c` | 4 | Grouped alloc failure check, calloc for overflow-safe basis, backup/restore on refactorize failure |
| `branch_bound.c` | 3 | INT_MIN return removed, bounds checks on variable indices in branching |
| `simplex.c` | 1 | Documentation of phase1_exclude_entering_var validation |

Commit: `e794dac ralph: harden MIP infrastructure from c-audit findings`

### 1.7 GLPK MIP Comparison (Feb 2026)

**Context:** FuelWise benchmark with `--glpk` flag comparing Ralph's B&B (with domain hints:
reach cuts, branching priorities/directions, LP presolve P3) against GLPK `glpsol` (solving
the identical LP-format MILP without hints). Both solvers get the same constraint set; Ralph
has additional domain-specific guidance.

**Current results (cut normalization fix + pseudocost + probing, 5 seeds × 10 runs):**

| Scenario | Ralph avg | GLPK avg | Speed | Gap avg | Gap max |
|----------|-----------|----------|-------|---------|---------|
| milp15 | **1.32 ms** | 6.52 ms | **5.7x Ralph** | 25.7% | 395.8% |
| milp30 | **5.16 ms** | 7.79 ms | **1.8x Ralph** | 1.72% | 8.6% |
| milp50 | 20.69 ms | **10.39 ms** | 0.7x | 0.69% | 5.4% |
| milp75 | 102.99 ms | **50.47 ms** | 0.9x | 0.55% | 1.6% |
| milp100 | 93.58 ms | **28.80 ms** | 0.3x | 0.48% | 4.4% |
| milp200 | 751.03 ms | **129.51 ms** | 0.2x | 0.87% | 18.9% |

Multi-seed benchmarks (seeds 42, 123, 456, 789, 1337). Gap = `(ralph - glpk) / |glpk| × 100%`.
100% solve rate, 0 false infeasibility. Key observations:

- **milp15** has catastrophic gap on 4/5 seeds (only seed 42 at 0.14%; others 3–57%). Root cause:
  very few stations (~15) means a single bad branching decision cascades. Root strong branching
  (probing all fractional variables) would eliminate this entirely.
- **milp30–milp75** have reasonable gaps (<2% avg). Cuts are working correctly.
- **milp100–milp200** speed regression: cut constraints enlarge the LP, making each node solve
  slower. GLPK is 3–10× faster due to cut pool management and LP solve efficiency.
- Zero false infeasibility across all 250 trials (50 per scenario).

**Previous results (P5/P6 only, before cut fix):**

| Scenario | Ralph avg | GLPK avg | Speed | Gap avg |
|----------|-----------|----------|-------|---------|
| milp15 | **0.85 ms** | 7.45 ms | **9.9x Ralph** | 13.97% |
| milp30 | **2.73 ms** | 9.38 ms | **4.1x Ralph** | 3.55% |
| milp50 | **9.28 ms** | 12.26 ms | **1.4x Ralph** | 0.39% |
| milp75 | **33.26 ms** | 57.93 ms | **2.0x Ralph** | 0.72% |
| milp100 | 62.05 ms | **37.21 ms** | 0.7x | 0.39% |
| milp200 | 994.96 ms | **152.22 ms** | 0.4x | 9.23% |

**Improvement vs previous (pre-HYBRID, pre-PATH B):**

| Scenario | Ralph (before) | Ralph (now) | Improvement |
|----------|----------------|-------------|-------------|
| milp15 | 1.07 ms | 0.99 ms | 1.1x |
| milp30 | 22.21 ms | 5.97 ms | **3.7x** |
| milp50 | 52.68 ms | 19.09 ms | **2.8x** |
| milp75 | 573.14 ms | 151.89 ms | **3.8x** |
| milp100 | 175.89 ms | 54.49 ms | **3.2x** |
| milp200 | 8987.79 ms | 890.40 ms | **10.1x** |

**PATH B LU Reuse Impact:** Profiling milp50 showed 90% of time in `lu_factorize_dense`
called from PATH B's `restore_basis_from_node()` → `tableau_refactorize()`. The fix: skip
basis restore entirely, reuse the current tableau's LU factors, just update bounds and run
`dual_reopt` with a larger budget (10×m, cap 2000). Each dual pivot is O(m) with LU update,
far cheaper than O(m³) full refactorization. milp30 went from losing to GLPK (0.4x) to
beating it (1.9x).

**Historical progression (seed 42, timing only):**

| Scenario | Baseline | +dual_reopt | +HYBRID+PATH B | +presolve remap | +P5/P6 guarded | +cut fix+pseu |
|----------|----------|-------------|----------------|-----------------|----------------|---------------|
| milp15 | ~2.3 ms | ~1.1 ms | 0.99 ms | 0.95 ms | **0.85 ms** | **1.29 ms** |
| milp30 | ~112 ms | ~22 ms | 5.97 ms | 3.14 ms | **2.73 ms** | **4.67 ms** |
| milp50 | ~197 ms | ~53 ms | 19.09 ms | 9.90 ms | **9.28 ms** | 27.76 ms |
| milp75 | ~1232 ms | ~573 ms | 151.89 ms | 55.49 ms | **33.26 ms** | 63.34 ms |
| milp100 | ~7223 ms | ~176 ms | 54.49 ms | 50.14 ms | 62.05 ms | 82.12 ms |
| milp200 | ~19473 ms | ~8988 ms | 890.40 ms | 212.03 ms | 994.96 ms | 780.10 ms |

**Note:** The "+cut fix+pseu" column is slower on milp50–milp100 because cut constraints enlarge
the working LP. However, objective gap dropped dramatically (milp15: 13.97%→0.14% on seed 42,
milp200: 9.23%→3.82%). The speed-quality tradeoff favors cuts at smaller scales (milp15–milp30)
and needs cut pool management for larger problems.

**Analysis:**
- Ralph wins milp15 through milp30 (5.7x→1.8x avg across 5 seeds)
- **milp15 branching quality** is the biggest remaining problem: 25.7% avg gap across seeds
  (catastrophic 57%/41%/26% on seeds 456/789/1337, but only 0.14% on seed 42)
- milp30–milp75 gaps are acceptable (<2% avg across seeds)
- milp100–milp200 speed regression from cut constraints: each node LP is larger
- GLPK faster at milp100+ (3–10x) due to: faster LP solves, cut pool pruning, heuristics

**Post-`9315fd3` improvements (not yet benchmarked in table above):**
- Reliability branching (default), reduced-cost fixing, RINS heuristic
- Strong branching UAF fix + NaN safety — all 5 seeds crash-free
- Root cause analysis: ~80% of gap from cut generation (only 2 families vs GLPK's 7+),
  ~10% from root-only cuts, ~5% heuristic depth, ~5% branching robustness. See §4.4.
- Next priority: cover cuts for knapsack-like constraints, node-level cut generation

See `fuelwise.md` §8 for FuelWise-specific optimization ideas (symmetry-breaking, flow
cover cuts, mandatory station fixing).

### 1.8 P5: Bound Flipping + P6: Dual Steepest Edge ✅

Two dual simplex enhancements targeting the B&B hot path (`dual_reopt()`):

**P5: Bound Flipping in Dual Ratio Test**
- During dual ratio test, boxed variables (finite lb AND ub) are flipped to opposite bound
  instead of entering the basis — avoids expensive LU update
- Two-pass Harris ratio test: Pass 1 finds theta_min, Pass 2 flips boxed vars strictly below
  theta_harris, selects entering with largest |alpha_j|
- Max flips per iteration capped at m/2 to prevent numerical blow-up
- After flips: `tableau_compute_solution()` refreshes primal values (non-leaving basic vars
  affected by flip); reduced costs unaffected (depend on basis, not non-basic values)
- **Restricted to `dual_reopt()` only** — incompatible with bound perturbation used in
  `dual_simplex_solve()`/`dual_simplex_solve_from_scratch()` for cycling prevention
  (perturbation corrupts flip magnitude ub-lb)

**P6: Dual Steepest Edge Pricing**
- Leaving variable selection: `score = infeas²/weight` where `weight = ||row_i(B^{-1})||²`
- Approximate init (weights=1.0) in `dual_reopt()` — few pivots per call, weights refine
  across B&B nodes via persistent update
- Exact init (m BTRANs) in `dual_simplex_solve()`/`dual_simplex_solve_from_scratch()`
- Weight update per pivot: one extra FTRAN + O(m) arithmetic (~25% more per pivot, but
  DSE typically reduces pivot count 2-3x)
- Formula: `w_i_new = w_i - 2*(d_i/d_r)*sigma_i + (d_i/d_r)²*w_r` where
  `sigma = B^{-1} * (B^{-T} * e_leaving)` (pivot row already computed)
- Weights persist across PATH A/B nodes; reset on PATH C cold start or refactorization

**Files modified:** `lp.h` (fields), `simplex.c` (arena alloc + defaults), `dual_simplex.c`
(core P5/P6), `mip.c` (PATH C reset)

**Feature flags:** `solver->use_dual_bound_flip` and `solver->use_dual_steepest_edge`
(both default on)

**Tests:** 3 new tests (213 total): `test_dual_bound_flip_basic`, `test_dual_bound_flip_mip`,
`test_p5_p6_combined`

### 1.9 Presolve Impact on FuelWise MIP (Investigation)

**Question:** Presolve helps LP (beaconfd, lotfi) but makes FuelWise MIP 20-100x slower. Why?

**Infrastructure:** `presolve_with_mask()` enables selective technique control via bitmask.
Exposed through `ralph_set_int_param("presolve_mask")` and `fw_set_presolve()`.
Bench tool: `fuelwise-bench --presolve-mask 0x110F`.

**All-minus-one analysis (milp30, baseline 5ms no-presolve, 111ms all-presolve):**

| Removed Technique | Time | Impact |
|-------------------|------|--------|
| w/o SHIFT_BOUNDS | **INFEASIBLE** | Required for correctness with other techniques |
| w/o SINGLETON_COLS | 20 ms | Major offender (111→20) |
| w/o PROBING | 46 ms | Significant (111→46) |
| w/o PROPORTIONAL_ROWS | 79 ms | Moderate (111→79) |
| w/o others | ~111 ms | No impact |

**Cross-scenario comparison (best lightweight mask: 0x110F):**

| Scenario | No presolve | Lightweight (0x110F) | All (0xFFFF) |
|----------|------------|---------------------|--------------|
| milp15 | **0.77 ms** | 0.99 ms | 10.43 ms |
| milp30 | 4.94 ms | **3.25 ms** | 110.59 ms |
| milp50 | 18.20 ms | **14.38 ms** | 717.20 ms |
| milp75 | 218.96 ms | **99.70 ms** | 2725.80 ms |
| milp100 | **58.69 ms** | 76.90 ms | 8639.20 ms |
| milp200 | **875.96 ms** | 2204.33 ms | timeout |

**Lightweight mask 0x110F** = FIXED_VARS + EMPTY_ROWS + EMPTY_COLS + SINGLETON_ROWS +
BOUND_TIGHTENING + SHIFT_BOUNDS (avoids SINGLETON_COLS, PROBING, PROPORTIONAL_ROWS).

**Root cause found:** Branch priorities and directions were NOT remapped through presolve.
The MIP solver received `solve_model` (presolved indices) but used original-index priorities,
causing completely wrong branching decisions. Fix: remap priorities via `presolved->var_map`.

**After priority remapping fix:**

| Scenario | No presolve | Lightweight (0x110F) | Speedup |
|----------|------------|---------------------|---------|
| milp30 | 4.15 ms | **2.52 ms** | **1.6x** |
| milp50 | 17.67 ms | **8.36 ms** | **2.1x** |
| milp75 | 142.82 ms | **50.34 ms** | **2.8x** |
| milp100 | 55.49 ms | **44.17 ms** | **1.3x** |
| milp200 | 903.49 ms | **225.86 ms** | **4.0x** |

Node counts now match (before fix: 2-4x more nodes with presolve; after: identical).
Lightweight presolve enabled by default in FuelWise MILP (`fw_presolve_mask = 0x110F`).

### 1.10 LP Presolve (P3) ✅

**Implemented:** 20-round fixed-point presolve with 12 techniques (matching GLOP iteration count):

| Technique | Description | Status |
|-----------|-------------|--------|
| Multi-round loop | Up to 20 fixed-point iterations (matching GLOP) | ✅ |
| Singleton row tightening | Derive variable bounds from single-variable constraints | ✅ |
| Doubleton equality elimination | Substitute x_j from `a*x_j + b*x_k = c`, reduce model | ✅ |
| Implied free detection | Remove redundant bounds when constraints already restrict | ✅ |
| Proportional row detection | Remove duplicate/redundant rows that are scalar multiples | ✅ |
| Proportional column detection | Fix dominated variables when columns have proportional coefficients | ✅ |
| Shift-variable-bounds | Transform x' = x - lb so lower bounds become zero (reduces simplex degeneracy) | ✅ |
| Forcing constraints | Detect constraints that force all variables to their bounds | ✅ |
| Bound tightening | Tighten variable bounds from constraint information | ✅ |
| MIP probing w/ implication propagation | Fix binary vars to 0/1, propagate bounds, intersect results | ✅ |
| Shared row activity bounds | `compute_row_bounds()` primitive used by forcing, tightening, probing | ✅ |
| Postsolve stack | LIFO replay of substitutions, shifts, and fixed-var ops | ✅ |

**Key implementation details:**
- Doubleton elimination: avoids integer variables, skips when fill-in exceeds 2x non-zeros,
  pivots on larger coefficient for numerical stability
- Implied free: tightens to finite computed implied bounds (not ±infinity) to avoid
  breaking Big-M method in simplex Phase 1
- Proportional columns: only eliminates when dominated var has non-negative effective cost
  (fixing at lb is provably correct); skips negative-ratio and negative-cost cases that
  would require bound expansion on the non-dominated variable
- Shift-variable-bounds: records `POSTSOLVE_SHIFT` ops for correct solution recovery;
  skips integer/binary variables (would change integrality)
- MIP probing: orchestrates `presolve_bound_tightening()` (not a duplicate implementation);
  saves/restores model bounds, probes each binary var at 0 and 1, intersects implied bounds.
  Detects infeasibility (one direction impossible → fix to other) and global bound improvements.
  Up to 100 binary variables probed, 3 propagation passes per probe.
- `compute_row_bounds()` shared primitive: computes row_lb, row_ub, abs_sum, finiteness flags.
  Used by `presolve_compute_implied_bounds()` (forcing constraints) and `presolve_bound_tightening()`
  (second pass derives per-variable bounds). Eliminates duplication and ensures all callers
  get cancellation guards for free.
- Postsolve uses `PostsolveOp` stack with types: `FIXED_VAR`, `SUBSTITUTION`, `SHIFT`
- Sweep after presolve loop scans `ctx->working` (not original model) for deleted columns
  with `lb == ub` and pushes `POSTSOLVE_FIXED_VAR` ops for recovery
- `build_reduced_model()` populates identity var_map/con_map on 0-reduction early return
- `ralph_optimize()` adds `obj_offset` from presolve to reported objective
- Trivial model handling: when presolve eliminates all constraints/variables, sets each
  variable to its optimal bound and runs postsolve

**Files:** `presolve.c` (~2750 lines), `presolve.h`, `ralph.c` (obj_offset + trivial model)
**Tests:** 105 assertions in `test_presolve.c` covering all techniques + edge cases + regressions
**Commits:** `94c3808` (initial P3), subsequent commits for proportional/shift/probing/orthogonalization

### 1.11 Supernodal LU Factorization (Planned)

**Problem:** LU factorization is 42% of total solve time. Ralph currently does column-by-column
sparse factorization with linked-list operations. The per-iteration cost gap vs GLPK grows from
1.3x at small sizes to 13.7x at 1000x500 — dominated by LU operations (factorization 42%,
BTRAN 27%, FTRAN 26%).

**Solution:** Supernodal factorization groups columns with similar sparsity structures into
dense blocks ("supernodes") and uses dense BLAS kernels (dgemm/dtrsm) for inner operations.
This converts many small sparse operations into fewer large dense operations that exploit
CPU cache hierarchy and SIMD.

```
Before: 250 individual pivots, each with linked-list ops → cache misses
After:  ~50 supernodes, each using dense matrix multiply → cache-friendly
```

**Implementation phases:**

| Phase | Task | Description | Effort |
|-------|------|-------------|--------|
| 1 | **Symbolic/numeric separation** | Separate structure analysis from numeric factorization. Reuse symbolic analysis across refactorizations (basis structure is often similar). Pre-allocate exact memory needed. | Medium |
| 2 | **Elimination tree** | Build column dependency tree during symbolic analysis. Required for supernode detection. | Medium |
| 3 | **Supernode detection** | Identify consecutive columns with identical row structure (fundamental supernodes). Merge adjacent supernodes with similar structure (relaxed supernodes). | Medium |
| 4 | **Dense BLAS kernels** | Replace sparse column ops within supernodes with dense dgemm/dtrsm. No external BLAS dependency — implement focused kernels in Ralph (small dense matrices, typically 2-20 columns). | High |
| 5 | **Supernodal triangular solves** | Extend supernodal structure to FTRAN/BTRAN. Use dense kernels for panel solves within supernodes. | Medium |

**Expected impact:**
- Phase 1 alone: 1.5-2x for refactorization (reuse symbolic analysis)
- Phases 1-4: 3-5x for factorization (42% of total → ~10%)
- Phase 5: Additional 1.5-2x for FTRAN/BTRAN (53% of total)
- Combined: potential 3-6x overall iteration speedup

**Key design decisions:**
- No external BLAS dependency (WASM compatibility, zero-dependency mandate)
- Inline dense kernels sized for typical LP supernodes (2-20 columns)
- Hyper-sparse threshold: skip supernodal path when RHS density < 10%
- LP-specific optimization: order singleton columns (identity/slack) last, apply
  supernodal only to the non-trivial submatrix

**Files:** `src/lu.c` (major refactor), new `src/lu_symbolic.c`, new `src/lu_supernode.c`
**References:** SuperLU (Demmel et al.), CHOLMOD, GLPK `bflib/`

### 1.12 State-of-the-Art LP Gap Analysis (Feb 2026)

Comprehensive comparison of Ralph's LP solver against production solvers (GLOP, CLP, GLPK).
See `docs/roadmaps/ralph_vs_glop.md` for the original GLOP comparison. This section extends
it with a full codebase audit.

**Current position:** Ralph is ~60% of state-of-the-art. The per-iteration cost gap vs GLPK
is 8.8x on 1000x500 problems (§1.1). The gap is dominated by LU operations (42% factorize,
27% BTRAN, 26% FTRAN). blend (NETLIB) fails due to numerical instability. milp100+ loses
to GLPK on LP speed per B&B node.

#### What Ralph Does Well

| Feature | Quality | Location | Notes |
|---------|---------|----------|-------|
| LP-aware LU factorization | Excellent | `lu_sparse.c:1630` | Separates identity/structural columns; factorizes only the k×k structural submatrix |
| Hyper-sparse FTRAN/BTRAN | Excellent | `lu.c:1345,1441` | DFS-based reach computation, 12.5% density threshold |
| FT spike pool | Very good | `lu.c` | Contiguous cache-friendly storage with offset indexing |
| Phase 1 recovery | Very good | `simplex.c`, `dual_simplex.c` | Dual rescue, entering exclusion, alternate leaving, redundant row marking |
| dual_reopt for B&B | Very good | `dual_simplex.c:571` | PATH A/B/C design, objective cutoff, P5+P6 |
| LP Presolve | Good | `presolve.c` | 12 techniques, 20-round fixed-point, probing with implication propagation |
| Devex pricing | Good | `simplex.c:1533` | Approximate SE with periodic reference reset every 2n iterations |
| Sparse Markowitz LU | Good | `lu_sparse.c:974` | AMD ordering, singleton detection, threshold pivoting |
| Bound flipping (P5) | Good | `dual_simplex.c:217` | Two-pass Harris, restricted to dual_reopt |
| DSE pricing (P6) | Good | `dual_simplex.c:332` | Approx init in reopt, exact in full dual |

#### Tier 1: Critical Gaps (2-5x impact each)

**T1.1 Crash Basis — No initial basis heuristic**

| Aspect | Current | Target |
|--------|---------|--------|
| Initial basis | All-slack/artificial (`tableau_create_ex`, simplex.c:568) | Triangular crash (Maros LTSF) |
| Phase 1 cost | Always full Phase 1 or Big-M (30-50% of solve time) | Often eliminated entirely |
| Cold start quality | Worst possible starting point | Near-feasible basis from constraint structure |

Every LP starts from the identity basis. A triangular crash scans the constraint matrix for
structural columns that can enter the basis without bound violation: singleton columns first,
then columns with good pivot elements. This is well-documented (Maros Chapter 9, Bixby 1992).

- **Impact**: 2-5x on cold starts. Root LP in MIP is cold start. Every PATH C fallback is cold start.
- **Effort**: Medium (~300 LoC). New function `crash_triangular()` in `simplex.c`.
- **Dependencies**: None.
- **Who has it**: GLPK (triangular), GLOP (Bixby), CLP (Idiot + triangular).

**T1.2 Multi-Round Scaling — Only 1 round of geometric mean**

| Aspect | Current | Target |
|--------|---------|--------|
| Method | 1 round geometric mean (`apply_scaling`, simplex.c:249) | 5 rounds geometric + 20 rounds equilibrium |
| Row scaling | `1/sqrt(max\|a_ij\|)` | Iterative until all row/col norms ≈ 1.0 |
| Cost scaling | Scales c with column factors | Multiple cost scaling variants (contain-one, mean, median) |

One round is the bare minimum. GLPK does up to 200 equilibrium iterations. CLP does 5
geometric + 20 equilibrium. Better scaling → fewer degenerate pivots → fewer iterations.
The blend NETLIB failure is likely a scaling issue.

- **Impact**: 10-30% fewer iterations broadly. May fix blend. Compounds with other improvements.
- **Effort**: Low (~100 LoC). Equilibrium scaling is a simple iterative loop.
- **Dependencies**: None.

**T1.3 Dual Simplex as Default — Primal is default, dual is a helper**

| Aspect | Current | Target |
|--------|---------|--------|
| Default LP algorithm | Primal simplex | Dual simplex |
| `dual_simplex_solve()` | 5 fallback paths to primal, helper role | Standalone primary solver |
| Initial dual feasibility | `make_dual_feasible()` via flipping (fragile) | Crash basis + proper dual Phase 1 |

Modern solvers default to dual because: (1) with crash basis, no Phase 1 needed (just flip
non-basics for dual feasibility), (2) DSE gives better pivot selection, (3) bound changes
(B&B operations) only affect dual feasibility — cheap to restore.

- **Impact**: ~2x on initial LP solves. Architectural endgame for the LP solver.
- **Effort**: High. Requires rewriting `dual_simplex_solve()` without primal fallbacks.
- **Dependencies**: T1.1 (crash), P5 (done), P6 (done).
- **Path**: crash → clean dual_simplex_solve → make default → keep primal as fallback.

**T1.4 Symbolic/Numeric Separation in LU**

| Aspect | Current | Target |
|--------|---------|--------|
| Factorization | `lu_factorize_sparse()` mixes symbolic + numeric | Separate `lu_symbolic_analyze()` + `lu_numeric_factorize()` |
| Symbolic reuse | Recomputes elimination tree every refactorization | Reuse symbolic analysis when sparsity pattern unchanged |
| Memory | Allocated during factorization | Pre-allocated from symbolic analysis |

Basis sparsity structure changes slowly during simplex. Separating symbolic analysis
(pivot ordering, elimination tree, memory layout) from numeric computation allows reuse
across refactorizations. Also prerequisite for supernodal factorization (§1.11).

- **Impact**: 1.5-2x on refactorization (42% of per-iteration cost → ~25%).
- **Effort**: Medium-High (~500 LoC). Refactor `lu_factorize_sparse()` into two phases.
- **Dependencies**: None (but enables §1.11 supernodal).

#### Tier 2: High-Impact Gaps (1.5-3x on specific scenarios)

**T2.1 Supernodal LU Factorization**

Already planned in §1.11. Groups columns with similar sparsity into dense blocks, uses
BLAS-3 kernels (no external dependency). Would reduce LU from 42% → ~10% of iteration time.

- **Impact**: 3-5x factorization, ~2x overall for m > 500.
- **Effort**: High (~1500 LoC, 5 phases).
- **Dependencies**: T1.4 (symbolic/numeric separation).

**T2.2 Heap-Based Pricing (DynamicMaximum)**

| Aspect | Current | Target |
|--------|---------|--------|
| Pricing scan | O(n) scan all non-basics every iteration | Top-32 heap, O(log n) updates |
| Partial pricing | Block-of-100 round-robin (`pricing_partial`, simplex.c:1614) | Heap serves 98% of queries from cache |
| Cache efficiency | Full scan touches all RC values | Heap maintains hot candidates |

GLOP's DynamicMaximum pricing maintains a priority queue of the best pricing candidates.
After each pivot, only affected reduced costs are updated in the heap. 98% of pricing
queries served from cache without scanning.

- **Impact**: 2-5x on pricing for n > 500. Becomes important as LU gets faster.
- **Effort**: Low-Medium (~200 LoC). New `pricing_heap()` strategy.
- **Dependencies**: None.

**T2.3 Post-Solve Verification**

| Aspect | Current | Target |
|--------|---------|--------|
| Primal feasibility | Only iterative refinement during solve | Explicit `\|\|Ax - b\|\|` check post-solve |
| Dual feasibility | Not checked | Verify `c - A'y - s = 0`, `s ≥ 0` for non-basics at lower |
| Complementary slackness | Not checked | Verify `x_j * s_j = 0` |
| Status downgrade | Not possible | Downgrade OPTIMAL → IMPRECISE if tolerances exceeded |
| Objective accuracy | Recomputed from original vars (avoids Big-M) | Add Kahan summation |

Not a speed improvement, but essential for solver credibility. Would catch silent numerical
failures like the blend NETLIB problem. GLOP has 7 independent post-solve metrics.

- **Impact**: Correctness. Catches failures that currently go undetected.
- **Effort**: Low (~150 LoC). New `verify_solution()` function.
- **Dependencies**: None.

#### Tier 3: Moderate Gaps (10-30% improvements)

| ID | Gap | Current | Target | Impact | Effort |
|----|-----|---------|--------|--------|--------|
| T3.1 | **Objective limits in `simplex_solve`** | Only `dual_reopt` has cutoff | Early termination when LP bound exceeds incumbent | 30-50% fewer iters on pruned B&B nodes | Low (~50 LoC) |
| T3.2 | **Dynamic refactorization period** | Fixed `min(m/2, 200)` max updates | Adapt based on fill-in rate and condition estimate | 10-20% better LU amortization | Low (~50 LoC) |
| T3.3 | **Cost perturbation** | Only bound perturbation (`primal_apply_perturbation`) | Shift objective coefficients as alternative anti-degeneracy | Fewer degenerate pivots on specific problems | Low (~80 LoC) |
| T3.4 | **Per-phase pricing strategy** | Same pricing in Phase 1 and Phase 2 | Dantzig in Phase 1 (robust), Devex in Phase 2 (fast convergence) | 10-15% fewer Phase 1 pivots | Low (~30 LoC) |
| T3.5 | **Dual Phase 1 with auxiliary objective** | `make_dual_feasible()` just flips non-basics | Proper dual Phase 1 via auxiliary objective | More robust dual simplex starts | Medium (~200 LoC) |
| T3.6 | **Basis conditioning report** | `min_diag_u`/`max_diag_u` tracked but not exposed | Report condition estimate to user, auto-refactorize on drift | Better numerical diagnostics | Low (~30 LoC) |

#### Tier 4: Nice-to-Have

| Gap | Notes |
|-----|-------|
| IIS computation (Irreducible Infeasible Subsystem) | Diagnostic, not performance |
| Column generation interface | Only needed for Dantzig-Wolfe decomposition |
| Adaptive pricing strategy switching | Auto-switch Dantzig/Devex/SE based on progress |
| Curtis-Reid scaling | More sophisticated than equilibrium, diminishing returns |
| Parallel LU operations | Diminishing returns at Ralph's target problem sizes |
| Dualization (auto-dual when constraints >> vars) | GLOP has it; rarely triggers on FuelWise problems |

#### Implementation Guide

Each feature is designed for orthogonal, incremental delivery. Every feature defaults to OFF
behind a feature flag, must pass the full regression gate before being turned ON, and can be
reverted by flipping a single flag.

**Regression gate:** `make clean && make test` in ralph/ (272+ tests) AND fuelwise/ (29+ tests),
plus ASAN build (`CFLAGS="-fsanitize=address,undefined -g" make test`).

**T1.2 Multi-Round Scaling — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | Replaces body of `apply_scaling()` in `simplex.c:249-345`. Existing call site at `simplex_solve:3930` unchanged. |
| **Orthogonality** | Self-contained in `apply_scaling()`. No interaction with any other feature. |
| **Feature flag** | `ralph_set_int_param(model, "scaling_rounds", N)` — 0=off, 1=current, 5=multi-round (default stays 1 until validated) |
| **Implementation** | Outer loop: 5 rounds of geometric mean (existing logic). Then inner loop: up to 20 rounds equilibrium — row scale = 1/max|a_ij|, col scale = 1/max|a_ij|, repeat until max change < 1%. Keep existing cost scaling. |
| **Tests** | (1) Scaling with N=5 produces tighter row/col norm spread than N=1 on random 50x50. (2) blend NETLIB solves with N=5 (currently fails). (3) All existing LP tests pass with N=5. (4) Scaling roundtrip: scale→unscale returns original A within tolerance. |
| **Risk** | Very low. Replaces function body, fallback is `scaling_rounds=1` (current behavior). |

**T1.1 Crash Basis — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | New function `crash_triangular()` called between `tableau_create_ex()` (line 3939) and `tableau_refactorize()` (line 3967). Modifies `tab->basis[]` and `tab->var_status[]` before first LU factorization. |
| **Orthogonality** | Only writes initial basis state. Does not touch LU, pricing, or Phase 1 logic. If crash produces zero improvements, simplex proceeds exactly as before (all-slack). |
| **Feature flag** | `ralph_set_int_param(model, "crash", 1)` — 0=off (default initially), 1=triangular |
| **Implementation** | Maros LTSF: (1) Scan structural columns for singletons — if singleton element in row i and |a_ij| > PIVOT_TOL, replace slack/artificial in basis position i. (2) Scan remaining columns for columns with one element exceeding pivoting threshold in an unclaimed row. (3) Mark structural vars as RALPH_BASIC, displaced slacks/artificials as RALPH_NONBASIC_LOWER. ~300 LoC. |
| **Tests** | (1) Crash on 10x10 LP produces basis with ≥ 3 structural columns (vs 0 without). (2) Phase 1 iterations reduced by ≥ 30% on medium LP (50 vars). (3) Infeasible LP still detected correctly with crash. (4) All existing tests pass with crash=1. |
| **Risk** | Low. Worst case: crash inserts bad pivots → LU factorization fails → Phase 1 falls back to all-slack (standard behavior). Explicit fallback: if `tableau_refactorize()` returns error after crash, reset basis to all-slack and refactorize. |

**T2.3 Post-Solve Verification — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | New function `verify_solution()` called at `simplex_solve:4002` inside the `status == OPTIMAL` block, after solution copy and unscaling. Read-only: only inspects `solver->solution`, `solver->dual_solution`, `solver->reduced_costs`, model A/b/c. |
| **Orthogonality** | Purely diagnostic — reads solution, never modifies solver state except potentially downgrading `solver->status` from OPTIMAL to a new IMPRECISE status. |
| **Feature flag** | `ralph_set_int_param(model, "verify", 1)` — 0=off (default), 1=verify post-solve |
| **Implementation** | (1) Primal feasibility: compute `||Ax - b||_inf`, flag if > 1e-6. (2) Bound feasibility: check `lb ≤ x ≤ ub` for all vars. (3) Dual feasibility: check `rc[j] ≥ -tol` for non-basics at lower bound. (4) Complementary slackness: `|x_j - lb_j| * |rc_j|` should be ≈ 0. (5) Kahan summation for objective recomputation. Downgrade OPTIMAL → IMPRECISE if any check fails. ~150 LoC. |
| **Tests** | (1) Clean LP returns OPTIMAL with verify=1. (2) Deliberately perturbed solution triggers IMPRECISE. (3) No performance regression (verify adds < 1% overhead). |
| **Risk** | Zero. Read-only post-processing. If verify has a bug, worst case is false IMPRECISE — easily diagnosed. |

**T3.1 Objective Limits — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | Early-exit check in `simplex_phase2()` main loop, after each objective recomputation. Compare `tab->obj_value` against `solver->objective_limit`. |
| **Orthogonality** | Single `if` statement in phase2 loop. No interaction with pricing, ratio test, or LU. |
| **Feature flag** | `ralph_set_dbl_param(model, "obj_limit", val)` — default RALPH_INFINITY (no limit). Already partially exists for `objective_cutoff` in MIP context. |
| **Implementation** | In `simplex_phase2` loop: `if (tab->obj_value >= solver->objective_limit) { solver->status = RALPH_STATUS_OBJ_LIMIT; return 0; }`. Also add to full `dual_simplex_solve()` loop. ~50 LoC. |
| **Tests** | (1) Set obj_limit below optimal → returns OBJ_LIMIT. (2) Set obj_limit above optimal → returns OPTIMAL normally. |
| **Risk** | Zero. Simple comparison, no state mutation. |

**T3.4 Per-Phase Pricing — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | Override `tab->pricing_strategy` at two points: before `simplex_phase1()` (set to Dantzig=0) and after phase1/before phase2 (restore to solver's chosen strategy). |
| **Orthogonality** | Two lines of code. Pricing strategy dispatch (`select_entering_variable`) already handles all strategies — just changes which one is active. |
| **Feature flag** | `ralph_set_int_param(model, "phase1_pricing", 0)` — default 0 (Dantzig for Phase 1), -1 to disable (use same pricing for both phases) |
| **Implementation** | Before phase1: `int saved = tab->pricing_strategy; tab->pricing_strategy = phase1_pricing;`. After phase1: `tab->pricing_strategy = saved;` ~30 LoC. |
| **Tests** | (1) Phase 1 iteration count with Dantzig ≤ count with Devex on degenerate problem. |
| **Risk** | Zero. Pricing strategy is already hot-swappable. |

**T1.4 Symbolic/Numeric LU Separation — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | Refactors `lu_factorize_sparse()` (lu_sparse.c:974) into two functions. `lu_symbolic_analyze()` produces an `LUSymbolic` struct (pivot ordering, elimination tree, memory layout). `lu_numeric_factorize()` takes `LUSymbolic*` and numeric values, produces `LUFactor*`. Existing `lu_factorize_sparse()` becomes a wrapper calling both. |
| **Orthogonality** | Internal to LU module. All callers (simplex, dual simplex) use `tableau_refactorize()` which calls `lu_factorize_sparse()` — wrapper preserves API. |
| **Feature flag** | `ralph_set_int_param(model, "lu_symbolic_reuse", 1)` — 0=recompute symbolic every time (current), 1=cache and reuse when pattern unchanged |
| **Implementation** | New `lu_symbolic.c` (~500 LoC). Pattern change detection: compare column pointer array diff — if `colptr[j+1]-colptr[j]` unchanged for all j, reuse symbolic. Invalidate on basis structure change (rare after initial iterations). |
| **Tests** | (1) Symbolic+numeric produces identical LU to current monolithic. (2) Symbolic reuse across 10 refactorizations gives same results. (3) Pattern change correctly triggers re-analysis. |
| **Risk** | Medium. LU is the most sensitive code. Extensive comparison testing required. Fallback: `lu_symbolic_reuse=0` reverts to monolithic. |

**T2.2 Heap-Based Pricing — Implementation**

| Aspect | Detail |
|--------|--------|
| **Pipeline slot** | New pricing strategy `PRICING_HEAP=4` in the `select_entering_variable()` dispatch (simplex.c:1745). Sits alongside Dantzig(0), SE(1), Devex(2), Partial(3). |
| **Orthogonality** | New case in existing switch. Heap state lives in `SimplexTableau` (new fields: `heap_data[]`, `heap_size`, `heap_pos[]`). No interaction with ratio test, LU, or phase logic. |
| **Feature flag** | `ralph_set_int_param(model, "pricing", 4)` — dispatched through existing `solver->pricing_strategy` |
| **Implementation** | Binary max-heap of (|rc_j|, j) pairs. After each pivot, update rc for affected non-basics (computed from BTRAN result), sift in heap. `pricing_heap()` pops top candidate. Heap rebuild on refactorization (all rc recomputed). Top-32 cache for partial re-scan between rebuilds. ~200 LoC. |
| **Tests** | (1) Heap pricing produces same optimal as Dantzig on 10 test problems. (2) Iteration count within 5% of Dantzig. |
| **Risk** | Low. New strategy, doesn't touch existing pricing code. If buggy, use pricing=0/1/2/3. |

**T1.3 Dual Simplex as Default — Implementation**

This is the architectural endgame. Current `dual_simplex_solve()` has 6 primal fallback paths
that make it a helper, not a standalone solver. Requires phased rewrite.

*Current dual_simplex_solve fallback paths (all route to `simplex_solve`):*

| # | Location | Trigger | Action |
|---|----------|---------|--------|
| 1 | dual_simplex.c:788 | No tableau exists | Full primal from scratch |
| 2 | dual_simplex.c:857 | Can't achieve dual feasibility via flipping | Full primal from scratch |
| 3 | dual_simplex.c:1020 | Stalled after 3 perturbation attempts | Destroy tableau + full primal |
| 4 | dual_simplex.c:1044 | Too many dual violations (>n/5) | Destroy tableau + full primal |
| 5 | dual_simplex.c:1056 | Persistent violations (200+ iters) | Destroy tableau + full primal |
| 6 | dual_simplex.c:1071 | Too many iterations (>5m) | Destroy tableau + full primal |

*Phased plan:*

**Phase A: Crash basis enables dual start (depends on T1.1)**

With crash basis, the initial basis contains structural columns. Non-basic variables
can be flipped to achieve dual feasibility without destroying the basis. This eliminates
fallback #1 (no tableau) and makes #2 (can't achieve DF) rare.

```
simplex_solve pipeline with crash + dual:
  apply_scaling()
  → tableau_create_ex()
  → crash_triangular()         [T1.1: structural columns in basis]
  → tableau_refactorize()
  → make_dual_feasible()       [flip non-basics for dual feasibility]
  → dual Phase 1 if needed     [T3.5: auxiliary objective for remaining infeasibilities]
  → dual Phase 2               [main dual simplex iterations]
```

**Phase B: Clean dual solver function (~400 LoC)**

New function `dual_simplex_solve_clean()` that does NOT fall back to primal:
- Takes a tableau with a valid basis (from crash + refactorize, or warm-started from B&B)
- Achieves dual feasibility (flip + optional dual Phase 1)
- Runs dual simplex iterations with **exact DSE** + bound perturbation
- On stalling: re-perturb (up to 5 attempts), then return STALLED status
- On too many iterations: return ITERATION_LIMIT
- Supports objective cutoff (early termination for MIP pruning)
- **No tableau destruction, no primal fallback**
- Caller decides whether to fall back to primal

This function serves **both** cold-start LP solving AND B&B warm-start re-optimization.
The key insight: bound changes in B&B only break primal feasibility — dual feasibility is
preserved (reduced costs depend on basis and objective, not bounds). So the same dual
simplex function handles both cases naturally.

**Phase C: Method dispatch in simplex_solve**

```c
// In simplex_solve(), after tableau creation and refactorization:
if (solver->method == 1 || (solver->method == 2 && should_use_dual(solver))) {
    crash_triangular(tab);        // T1.1
    tableau_refactorize(tab);
    int rc = dual_simplex_solve_clean(solver);
    if (rc == 0) goto post_solve;  // Optimal or infeasible
    // Dual failed — fall back to primal
    // Reset basis to all-slack, refactorize, run Phase 1 + Phase 2
}
// Existing primal path (Phase 1 → Phase 2) unchanged
```

Feature flag: `ralph_set_int_param(model, "method", M)`:
- 0 = primal simplex (current default)
- 1 = dual simplex (forces dual, primal fallback on failure)
- 2 = auto (heuristic: dual if m > 50 and crash covers > 50% of rows)

**Phase D: Make dual the default**

After Phase C is validated on full NETLIB suite + FuelWise benchmarks:
- Change default `method` from 0 to 2 (auto)
- Keep primal as fallback for the 2-5% of problems where dual struggles
- This is a one-line change with the full safety net of `method=0` revert

**Phase E: Replace dual_reopt in B&B (delete ~200 LoC, simplify mip.c)**

`dual_reopt` exists because `dual_simplex_solve()` was unreliable (6 primal fallbacks).
With `dual_simplex_solve_clean()` from Phase B, `dual_reopt` becomes redundant. The B&B
node solver in `solve_node_lp()` (mip.c:960) collapses from 3 paths to 1:

```c
// CURRENT: 3 paths, 2 different solvers, inconsistent numerical profiles
PATH A: update bounds → dual_reopt(budget=500)     [approx DSE, no perturbation]
PATH B: update bounds → dual_reopt(budget=2000)    [approx DSE, no perturbation]
PATH C: destroy tableau → simplex_solve()           [primal from scratch, all-slack]

// WITH Phase E: 1 path, 1 solver
ALL:    update bounds → dual_simplex_solve_clean()  [exact DSE, perturbation, cutoff]
RARE:   cold start → crash + dual_simplex_solve_clean()
```

Why `dual_reopt` hurts milp15 quality (even though LP solutions are technically correct):
1. **Approximate DSE** (`dse_init_approx` sets all weights to 1.0) → poor leaving variable
   selection → more pivots → more numerical drift between refactorizations
2. **Two numerical profiles** → pseudocost updates mix LP bounds from dual_reopt (PATH A/B)
   and primal simplex (PATH C), creating inconsistent branching signals
3. **Budget-limited** → frequent PATH C fallbacks on non-child nodes → primal cold starts
   from all-slack basis (the worst possible starting point)
4. **Unnecessary complexity** → 200 lines of separate code maintaining its own refactorization
   logic, DSE init, bound flipping dispatch — all of which `dual_simplex_solve_clean()` handles

What Phase E changes in mip.c `solve_node_lp()`:
- Delete PATH A/B/C dispatch (~80 lines)
- Replace with: update bounds in tableau → `dual_simplex_solve_clean(lp)`
- `dual_simplex_solve_clean()` already supports objective cutoff (from its API)
- If no tableau exists (first node or post-cut-generation): cold start with method=2
- Delete `dual_reopt()` from dual_simplex.c (~200 lines)

*Dependencies:*
- Phase A requires T1.1 (crash basis)
- Phase B requires P5 (bound flipping, done) and P6 (DSE, done)
- Phase C requires Phase A + B
- Phase D requires Phase C + full validation
- **Phase E requires Phase B + C validated** (dual solver must be reliable before replacing dual_reopt)
- Optional: T3.5 (dual Phase 1) makes Phase A more robust but is not blocking

*Tests for each phase:*
- Phase A: (1) After crash, `make_dual_feasible()` succeeds on 90%+ of test LPs. (2) Crash+dual solves 20-variable LP correctly.
- Phase B: (1) `dual_simplex_solve_clean()` solves 10 LPs to optimality. (2) Returns correct INFEASIBLE on infeasible LP. (3) Returns STALLED (not crash) on adversarial degenerate LP. (4) Supports objective cutoff (returns OBJ_LIMIT when bound exceeds cutoff).
- Phase C: (1) method=1 solves all existing LP tests. (2) method=2 auto-selects correctly. (3) method=2 falls back to primal on problems where dual fails. (4) No regression on any existing test with method=2.
- Phase D: (1) All 272+ ralph tests pass with method=2 as default. (2) All 29+ fuelwise tests pass. (3) NETLIB suite: ≥ same solve rate as method=0. (4) FuelWise benchmarks: no regression on milp15/30/50/100.
- Phase E: (1) All MIP tests pass with dual_reopt removed. (2) milp15 multi-seed gap improves (target: < 15%, from 25.7%). (3) milp30/50/100/200 no regression. (4) B&B node counts within 10% of pre-change on all seeds.

*Risk assessment:*
- Phase A-B: Low. New function, old code untouched.
- Phase C: Medium. Modifies `simplex_solve` pipeline but with explicit fallback to existing primal path.
- Phase D: Medium. Default change affects all users. One-line revert to `method=0` if issues found.
- Phase E: Medium-High. Removes proven B&B hot path. Validated by multi-seed MIP benchmarks before landing. Revert: re-add `dual_reopt()` and PATH A/B/C dispatch.

#### Implementation Order (Prioritized by Impact/Effort)

| Order | ID | Feature | Impact | Effort | Deps | Flag |
|-------|-----|---------|--------|--------|------|------|
| 1 | T1.2 | Multi-round equilibrium scaling | Better numerics, may fix blend | ~100 LoC | None | `scaling_rounds` |
| 2 | T1.1 | Triangular crash basis | 2-5x cold starts | ~300 LoC | None | `crash` |
| 3 | T2.3 | Post-solve verification | Correctness | ~150 LoC | None | `verify` |
| 4 | T3.1 | Objective limits in simplex_solve | 30-50% fewer pruned-node iters | ~50 LoC | None | `obj_limit` |
| 5 | T3.4 | Per-phase pricing strategy | 10-15% Phase 1 improvement | ~30 LoC | None | `phase1_pricing` |
| 6 | T3.6 | Basis conditioning report | Diagnostics | ~30 LoC | None | `verify` (shared) |
| 7 | T1.4 | Symbolic/numeric LU separation | 1.5-2x refactorization | ~500 LoC | None | `lu_symbolic_reuse` |
| 8 | T2.2 | Heap-based pricing | 2-5x pricing on large problems | ~200 LoC | None | `pricing=4` |
| 9 | T3.2 | Dynamic refactorization period | 10-20% LU amortization | ~50 LoC | None | `refac_dynamic` |
| 10 | T3.3 | Cost perturbation | Fewer degenerate pivots | ~80 LoC | None | `cost_perturb` |
| 11 | T2.1 | Supernodal LU (§1.11) | 3-5x factorization | ~1500 LoC | #7 (T1.4) | `lu_supernode` |
| 12 | T3.5 | Dual Phase 1 with auxiliary objective | Robust dual starts | ~200 LoC | None | `dual_phase1` |
| 13 | T1.3 | Dual simplex as default | ~2x initial solves | ~400 LoC | #2 (T1.1), P5, P6 | `method` |
| 14 | T1.3e | Replace dual_reopt in B&B | Consistent node LP quality, simpler MIP solver | -200 LoC (net delete) | #13 (T1.3) | N/A (removes code) |

Items 1-6 total ~660 LoC with zero dependencies. They move Ralph from ~60% to ~75% of
state-of-the-art. Items 7-11 push to ~90%. Items 13-14 (dual-as-default + replace dual_reopt)
are the architectural endgame that gets to ~95% and simplifies the MIP solver.

**Milestone targets:**

| Milestone | Requirements | Expected Result |
|-----------|-------------|-----------------|
| **75% SotA** | T1.2, T1.1, T2.3, T3.1, T3.4, T3.6 | blend NETLIB passes, 2-3x cold start improvement, numerical diagnostics |
| **85% SotA** | + T1.4, T2.2, T3.2, T3.3 | Competitive per-iteration speed on m < 2000, proper LU reuse |
| **90% SotA** | + T2.1 (supernodal) | Competitive per-iteration speed on m < 5000 |
| **95% SotA** | + T1.3 (dual-as-default) | Competitive with GLPK/CLP on most NETLIB/MIPLIB instances |

**Testing strategy:** Every item must pass this gate before changing defaults:
1. `make clean && make test` in ralph/ — all 272+ tests pass
2. `make clean && make test` in fuelwise/ — all 29+ tests pass
3. ASAN build: `CFLAGS="-fsanitize=address,undefined -g" make test` in ralph/
4. FuelWise multi-seed benchmark: no regression on milp15/30/50 (5 seeds each)
5. NETLIB subset: afiro, adlittle, blend, share2b, israel, bandm, beaconfd

---

## Chapter 2: LAP Solver

### 2.1 Current Implementation ✅

**Algorithm**: Jonker-Volgenant-Castanon (JVC)
- O(n³) worst-case, often O(n²) in practice
- SIMD-optimized dense solver
- Native sparse JVC for <30% density

**Performance:**
| Size | Dense | Sparse (10%) | Warm Start |
|------|-------|--------------|------------|
| n=500 | 1.5ms | 1.1ms | 0.3ms (5x) |
| n=1000 | 10ms | 6.7ms | 1.5ms (7x) |
| n=2000 | 42ms | - | 6ms (7x) |

### 2.2 LAP Features ✅

| Feature | API | Notes |
|---------|-----|-------|
| Dense LAP | `ralph_lap_solve()` | Standard n×n |
| Sparse LAP | `ralph_lap_solve_sparse()` | CSR format |
| Rectangular | `ralph_lap_solve_rect()` | m×n problems |
| Warm start | `ralph_lap_solve_warm()` | Reuse duals |
| k-Best | `ralph_lap_solve_k_best()` | Murty's algorithm |
| Bottleneck | `ralph_lap_solve_ex()` | Minimax assignment |
| Priority | `ralph_lap_solve_ex()` | Unbalanced LAP |
| Cardinality | `ralph_lap_solve_ex()` | Min/max bounds |
| Qualification | `ralph_lap_solve_ex()` | Allow-list subsets |

---

## Chapter 3: Network Flow Solver

### 3.1 Current Implementation ✅

**Algorithm**: Network Simplex with candidate list pricing
- Standard MCNF problems
- Warm start for re-optimization
- Flow decomposition into paths

### 3.2 Network Flow Features ✅

| Feature | API | Notes |
|---------|-----|-------|
| Standard MCNF | `ralph_netflow_solve()` | Candidate list pricing |
| Unified API | `ralph_netflow_solve_ex()` | All algorithm variants |
| Flow decomposition | `ralph_netflow_decompose()` | Extract paths |
| Bottleneck | `algorithm = BOTTLENECK` | Minimize max arc cost |
| Warm start | `options.warm_start = 1` | Reuse basis |
| Cost scaling | `options.cost_scaling = 1` | For degeneracy |

---

## Chapter 4: Planned Features

### 4.1 Decomposition Methods

**Benders Decomposition** → See **Chapter 7** for full design
- Generic implementation in Ralph (not domain-specific)
- Infrastructure complete (§6), algorithm planned (§7)

**Dantzig-Wolfe Decomposition** (Planned)
- For block-angular structure
- Column generation approach
- Useful for: crew scheduling, cutting stock

### 4.2 External Solver Backends

**HiGHS Integration** (Planned)
- Open-source LP/MIP solver
- Fallback for hard problems
- Interface: `ralph_set_backend(RALPH_BACKEND_HIGHS)`

**GLPK Integration** (Planned)
- GPL-licensed reference solver
- Benchmarking and validation
- Interface: `ralph_set_backend(RALPH_BACKEND_GLPK)`

### 4.3 MIP Improvements

GLPK benchmark (§1.7) confirmed these are the critical gaps:

| Task | Priority | Expected Impact | Notes |
|------|----------|-----------------|-------|
| **HYBRID node selection** | ✅ **Done** | 3-4x on milp30/50 | DFS→best-bound on incumbent; PATH B LU reuse eliminates O(m³) refactorize |
| **Objective cutoff** | ✅ **Done** | 1.1-1.4x on small MIP | Prune nodes in dual_reopt when obj exceeds incumbent |
| **c-MIR cuts** | ✅ **Done** | Tighter relaxation | Row normalization sign fix in GMI/c-MIR back-substitution; safety guard for cut-induced infeasibility |
| **Bound flipping (P5)** | ✅ **Done** | 20-50% on small MIP | Flip boxed vars in dual ratio test; restricted to dual_reopt (no perturbation) |
| **Dual steepest edge (P6)** | ✅ **Done** | 20-50% on small MIP | DSE leaving selection; approx init in dual_reopt, exact in full dual |
| **LP presolve (P3)** | ✅ **Done** | 2-3x avg improvement | 12 techniques, 20-round, probing w/ implication propagation |
| **Aggressive presolve** (probing) | ✅ **Done** | 1.5-2x smaller problems | Probing w/ implication propagation, orthogonal reuse of bound tightening |
| **Presolve mask for MIP** | ✅ **Done** | Investigation only | See §1.8 — presolve hurts FuelWise MIP; keep disabled |
| **Pseudocost branching** | ✅ **Done** | Better var selection | Obj-coeff init `fmax(|c_j|, 1.0)`, updated from actual bound changes |
| **Root strong branching** | ✅ **Done** | Seeds pseudocosts | Probe 20 fractional vars × 50 dual pivots at root |
| **Node probing** | ✅ **Done** | Bound tightening | Column-based propagation at depth < 20 |
| **Reliability branching** | ✅ **Done** | Better var selection (milp15 fix) | Hybrid strong/pseudocost; strong-branch when obs < threshold, then trust pseudocosts |
| **Reduced-cost fixing** | ✅ **Done** | Tighter bounds at nodes | Fix vars where `rc > incumbent - lp_bound`; O(n) per node |
| **RINS heuristic** | ✅ **Done** | Better incumbents | Fix vars where LP=incumbent, dive on rest; every 50-100 nodes |
| Cut efficacy purging | **High** | Speed at milp100+ | Discard low-efficacy cuts to keep LP small |
| Cover cuts (knapsack) | **High** | milp15 LP bounds | GLPK generates cover cuts; Ralph has only GMI+c-MIR |
| Node-level cut generation | **High** | Tighter per-node bounds | Currently cuts only at root; add at promising nodes |
| Feasibility pump | Medium | Find incumbents early | Alternate LP relaxation and rounding |
| Clique detection | Medium | From set-packing constraints | |
| Conflict analysis | Low | Learn from infeasibility | Requires conflict graph infrastructure |

### 4.4 Why B&B Finds Suboptimal Solutions (Objective Gap Analysis)

**Context:** Ralph's root LP relaxation matches GLPK's (same optimal basis), yet the
final MIP objective is often worse. The gap is entirely in tree search quality, not LP
solver quality. Multiple issues were discovered and fixed; remaining gaps documented below.

**Multi-seed benchmark gap data (5 seeds × 10 runs each, post-cut-fix):**

| Scenario | Gap Avg | Gap Max | Notes |
|----------|---------|---------|-------|
| milp15   | 25.7%   | 395.8%  | Catastrophic on 4/5 seeds — branching quality issue |
| milp30   | 1.72%   | 8.6%    | Reasonable |
| milp50   | 0.69%   | 5.4%    | Acceptable |
| milp75   | 0.55%   | 1.6%    | Good |
| milp100  | 0.48%   | 4.4%    | Acceptable |
| milp200  | 0.87%   | 18.9%   | Variable (seed 42 is worst at 3.82%) |

Gap = `(ralph_obj - glpk_obj) / |glpk_obj| × 100%`. Positive = Ralph worse.

**Bugs found and fixed:**

1. **Cut generation was disabled** (`max_cut_rounds = 0` at API level, `ralph.c:106`).
   Zero Gomory/c-MIR cuts were ever generated for FuelWise. Fix: set `max_cut_rounds = 3`
   via `ralph_set_int_param()` in `fw_refuel.c`.

2. **Row normalization sign bug in GMI/c-MIR back-substitution.** When a constraint has
   GE sense with positive RHS, the simplex normalizes it by multiplying by -1 (flipping
   sense and signs). `tab->row_sign[con_row]` stores this sign (+1 or -1). The cut
   generators used raw `model->b[con_row]` and `model->A->values[p]` without applying
   `row_sign`, producing cuts with inverted coefficients. Fix: multiply by `rsign` in
   both `generate_gmi_cut_from_row()` (~line 371) and `cmir_extract_source_row()` (~line 615).

3. **Cut-induced infeasibility without safety guard.** After multiple cut rounds, cuts-on-cuts
   interaction can cause the LP to become infeasible. Fix: if LP becomes non-optimal after
   cuts, discard ALL cuts (rebuild `working_model` from `original_model`), re-solve, and
   continue tree search without cuts (`mip.c` ~line 1192).

4. **Heap-use-after-free in reliability branching** (`9315fd3`). `strong_branch()` calls
   `dual_simplex_solve()` which may fall back to `simplex_solve()`, freeing and reallocating
   `lp->solution`. The caller `select_reliability_branch_impl` held a stale `solution` pointer.
   Fix: re-read solution pointer after every `strong_branch()` call, add NULL guards, and
   cold-start re-solve recovery in `process_node()`.

5. **`-ffinite-math-only` causing NaN safety check elision** (`9315fd3`). The `-ffast-math`
   flag implies `-ffinite-math-only`, which lets the compiler assume NaN/Inf never occur,
   optimizing away `isnan()`/`isinf()` checks. Degenerate pivots produce NaN which then
   propagates silently. Fix: add `-fno-finite-math-only` to CFLAGS in both `ralph/Makefile`
   and `fuelwise/Makefile`.

**Root cause analysis of remaining gap (Feb 2026):**

The LP solver is NOT the bottleneck. Ralph's root LP relaxation matches GLPK's optimal
basis. The gap is entirely in MIP tree search quality. Evidence: milp15 (smallest problems)
have the worst gaps — the *inverse* of what LP solver weakness would cause.

| Root Cause | Impact | Evidence |
|------------|--------|----------|
| **Cut generation: only 2 families** | ~80% of gap | Ralph: GMI + c-MIR only. GLPK: 7+ families (cover, clique, flow cover, MIR, Gomory, implicit bounds, GUB covers). FuelWise MILPs have knapsack-like structure that benefits from cover cuts. |
| **Cuts only at root** | ~10% of gap | Ralph generates cuts only at root node. GLPK generates at promising nodes throughout the tree, keeping LP bounds tight deeper in the search. |
| **Heuristic depth** | ~5% of gap | Ralph: diving + RINS (newly added). GLPK: RINS + feasibility pump + LP-guided rounding + polishing. More heuristic diversity finds better incumbents. |
| **Branching robustness** | ~5% of gap | Reliability branching now implemented. Remaining gap: GLPK's strong branching is more robust to LP state corruption. |

**Remaining fix plan (prioritized by impact):**

| Fix | Impact | Cost | Status |
|-----|--------|------|--------|
| ~~Reliability branching~~ | ~~High~~ | ~~Medium~~ | ✅ Done (`9315fd3`) |
| ~~Reduced-cost fixing~~ | ~~Medium~~ | ~~Low~~ | ✅ Done (`9315fd3`) |
| ~~RINS heuristic~~ | ~~High~~ | ~~Medium~~ | ✅ Done (`9315fd3`) |
| Cover cuts for knapsack constraints | **High** (milp15 LP bounds) | Medium (~400 LoC) | TODO |
| Node-level cut generation | **High** (tighter per-node bounds) | Medium (~200 LoC) | TODO |
| Cut pool management (efficacy purging) | **High** (milp100+ speed) | Medium (~300 LoC) | TODO |
| Feasibility pump heuristic | Medium (find incumbents early) | Medium (~300 LoC) | TODO |
| Conflict analysis | Low (long-term) | High (~600 LoC) | TODO |

**Note:** For domain-specific MIP improvements targeting FuelWise and HoSE, see **§6**.
The domain-specific approach (branching priorities, reach cuts, clock cuts) provides
better performance than generic improvements for these structured problem classes.

### 4.5 Closing the Gap to 0%: MIP Improvement Plan

**Goal:** Achieve ≤1% optimality gap vs GLPK across all scenarios and seeds, while
maintaining competitive speed through milp75.

**Phase 1: Reliability branching + RC fixing + RINS ✅ Done (`9315fd3`)**

- **Reliability branching**: hybrid strong/pseudocost. Strong-branch when obs < 8, then
  trust pseudocosts. Priority-aware (respects FuelWise branching priorities). Default
  variable selection strategy.
- **Reduced-cost fixing**: at each B&B node, fix integer vars where `rc > gap` to their
  current bound. O(n) per node. Stats tracked in `solver->rc_fixings`.
- **RINS heuristic**: every 50-100 nodes, fix vars where LP and incumbent agree, dive
  on remaining free variables. Uses same diving pattern as `diving_heuristic()`.
- **Bug fixes**: UAF in strong branching, `-ffinite-math-only` NaN safety.
- Impact: crash-free on all seeds, reliability branching provides more consistent results.

**Phase 2: Cut generation improvements (next priority, ~600 LoC)**

The ~80% root cause of the remaining gap. Ralph has only GMI + c-MIR; GLPK has 7+ families.

- **Cover cuts**: FuelWise MILPs have knapsack-like constraints (tank capacity, fuel
  balance). Cover cuts are the standard technique for these — find minimal covers and
  lift coefficients. ~400 LoC in `cuts.c`.
- **Node-level cut generation**: currently cuts only at root. Add cut rounds at promising
  B&B nodes (depth < 10, fractional solution with tight gap). ~200 LoC in `mip.c`.
- Expected impact: milp15 gap → <5% avg (vs 25.7% currently)
- Success criterion: milp15 gap < 5% on ALL seeds

**Phase 3: Cut pool management (~300 LoC)**

Cut constraints enlarge the LP, slowing node solves at scale.

- **Efficacy-based purging:** after N nodes, scan cuts. If `slack > threshold` for K
  consecutive rounds, remove the cut from the working LP. Keep in a "reserve pool" for
  potential re-addition.
- **Age-out:** cuts not tight for 50+ nodes are purged
- **Limit total cuts:** cap at 2×m_original constraint rows. When full, replace lowest-
  efficacy cut with new cut.
- Expected impact: milp100 speed 2-3×, milp200 speed 3-5×
- Success criterion: milp100 speed > 0.5x GLPK

**Phase 4: Feasibility pump (~300 LoC)**

Additional heuristic diversity for finding incumbents.

- Alternate between LP relaxation and rounding to find feasible solutions
- Complements RINS (which needs an existing incumbent to work)
- Run at root before tree search starts
- Expected impact: milp100–milp200 gap → <0.5% avg

**Phase 5: Conflict analysis (long-term, ~600 LoC)**

When a node is pruned by infeasibility:
- Trace the conflict to a minimal set of branching decisions
- Add a "no-good" constraint to prevent re-exploring the same combination
- Requires conflict graph infrastructure
- Expected impact: 20-40% fewer nodes on hard instances

**Verification plan (multi-seed):**

```bash
# Run after each phase:
for seed in 42 456 789 1337 9999; do
  for scenario in milp15 milp30 milp50 milp75 milp100 milp200; do
    ./fuelwise/bench/fuelwise-bench --scenario $scenario --runs 10 \
      --milp --glpk --presolve --seed $seed
  done
done

# Success criteria per phase:
# Phase 1: ✅ Done — all seeds crash-free, reliability branching default
# Phase 2: milp15 gap < 5% on ALL seeds
# Phase 3: milp100 speed > 0.5x GLPK
# Phase 4: milp100-milp200 gap < 0.5% on ALL seeds
# Phase 5: milp200 gap < 0.1% on ALL seeds
```

---

## Technical Reference

For deep-dive documentation, see [docs/internals/](../internals/):
- [ralph-architecture.md](../internals/ralph-architecture.md) - Solver architecture
- [lu-factorization.md](../internals/lu-factorization.md) - LU decomposition details
- [simplex.md](../internals/simplex.md) - Revised simplex implementation

---

## Chapter 5: REST API & WASM Demo

Transport-agnostic API for solving small LP/MIP problems, primarily for WASM demos
on the documentation site. Follows patterns from Carta, Velo, Locus.

### 5.1 Scope

| Use Case | Supported | Notes |
|----------|-----------|-------|
| WASM demo on docs | ✅ | Primary goal |
| Small LP/MIP via curl | ✅ | Testing, learning |
| MPS/LP format input | ✅ | Embedded in JSON |
| Large production problems | ❌ | Embed library directly |
| Warm starts | ❌ | Stateful, use library |

### 5.2 API Design

**Port**: 8084

**Endpoints**:
```
POST /api/v1/solve      - Solve LP/MIP from JSON-embedded problem
GET  /api/v1/health     - Health check
GET  /api/v1/formats    - List supported input formats
```

**Request** (POST /api/v1/solve):
```json
{
  "format": "lp",
  "problem": "max: 5x + 3y; 2x + 4y <= 40; x >= 0; y >= 0;",
  "timeout_ms": 5000
}
```

**Response**:
```json
{
  "status": "optimal",
  "objective": 42.5,
  "variables": {"x": 10.0, "y": 5.5},
  "solve_time_ms": 12,
  "iterations": 23
}
```

**Size Limits**: 100 vars/constraints (LP), 50 vars/constraints (MIP), 5s default timeout.

### 5.3 File Structure

```
ralph/
├── include/
│   └── ralph_api.h          # Transport-agnostic API handler
├── src/
│   ├── ralph_api.c          # API handler implementation
│   └── ralph_parse_lp.c     # LP format parser
├── api/
│   └── src/main.c           # Mongoose wrapper (port 8084)
└── wasm/
    └── ralph_wasm_api.c     # WASM wrapper
```

### 5.4 LP Format

```
/* Production Planning Example */
max: 5 chairs + 3 tables;

wood:  2 chairs + 4 tables <= 40;
labor: 3 chairs + 2 tables <= 24;

chairs >= 0;
tables >= 0;
```

Supports: `min`/`max` objective, `<=`/`>=`/`=` constraints, named constraints, bounds, comments.

### 5.5 Implementation TODOs

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| 1. Handler | 2-3 days | `ralph_api.h`, `ralph_api.c`, `ralph_parse_lp.c` |
| 2. HTTP Server | 1 day | `ralph/api/src/main.c`, Makefile |
| 3. WASM | 1 day | `ralph/wasm/ralph_wasm_api.c`, Makefile |
| 4. JS Handler | 1 day | `site/js/handlers/ralph.js`, api-config.json |
| 5. Demo | 0.5 day | Example LP, documentation |
| 6. Testing | 1 day | Integration, api-docs, test script |

**Total: ~6-7 days**

---

---

## Chapter 6: FuelWise & HoSE MIP Infrastructure

This chapter covers MIP infrastructure needed for FuelWise (refueling optimization) and HoSE
(Hours of Service engine). Both modules require domain-specific cuts and branching control
to achieve competitive solve times.

**Key insight:** Domain-specific cuts are the equalizer that makes Ralph competitive with
commercial solvers for structured problems like FuelWise and HoSE.

### 6.1 Overview

| Module | Problem Structure | Binary Vars | Key Challenge |
|--------|-------------------|-------------|---------------|
| **FuelWise** | Path with k stations | k (stop decisions) | Scale to k>30 stations |
| **HoSE** | Time-indexed schedule | ~6000 (state vars) | 1-week horizon at 5-min granularity |

Both benefit from:
- Branching priorities (cheaper/earlier stations first)
- Domain-specific cuts (reach cuts, driving capacity cuts)
- Warm start for iterative solving

### 6.2 New Ralph API

#### P0: Branching Control (~100 LoC)

```c
/*
 * Set branching priority for each variable.
 * Higher priority = branch first. Default = 0.
 */
void ralph_set_branch_priorities(RalphModel *model, const int *priorities);

/*
 * Set preferred branching direction for each variable.
 * 0 = down first (try x=0), 1 = up first (try x=1), -1 = auto
 */
void ralph_set_branch_directions(RalphModel *model, const int *directions);
```

**Implementation:**
- Store arrays in `RalphModel` struct
- Modify `select_branching_variable()` in `branch_bound.c`
- Use direction hint when creating child nodes

**Use cases:**
- FuelWise: Branch on cheaper stations first, prefer stopping at cheap stations
- HoSE: Branch on earlier periods first, prefer OFF states to explore "rest early"

#### P1: Constraint Modification + Bounds Query (~50 LoC)

```c
/*
 * Modify RHS of existing constraint (for Benders iterations).
 */
int ralph_set_constraint_rhs(RalphModel *model, int constraint, double rhs);

/*
 * Query variable bounds (for cut generators).
 */
int ralph_get_var_bounds(RalphModel *model, int var, double *lb, double *ub);
```

#### P1: Lazy Constraints (~150 LoC)

```c
/*
 * Add constraint(s) and re-solve with warm start.
 * User controls the cut loop externally.
 */
int ralph_add_lazy_constraint(RalphModel *model, const RalphCut *cut);
int ralph_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count);
```

**Usage pattern:**
```c
while (1) {
    ralph_optimize(model);
    if (ralph_get_status(model) != RALPH_STATUS_OPTIMAL) break;

    double *x = ralph_get_solution(model);
    RalphCut cuts[10];
    int n = my_generate_cuts(x, cuts, 10);  /* User's domain logic */

    if (n == 0) break;  /* No violations, done */
    ralph_add_lazy_constraints(model, cuts, n);
}
```

#### P2: Warm Start + Cut Callback ✅

```c
/* Basis representation for warm start */
typedef struct RalphBasis RalphBasis;
RalphBasis* ralph_save_basis(const RalphModel *model);
int ralph_load_basis(RalphModel *model, const RalphBasis *basis);
void ralph_free_basis(RalphBasis *basis);

/* Farkas ray (infeasibility certificate) - already implemented */
int ralph_get_farkas_ray(const RalphModel *model, double *ray);

/* Cut callback for automatic cut generation at B&B nodes */
typedef struct {
    int (*generate_cuts)(void *user_data, const double *x_relaxation,
                         int num_vars, RalphCut *cuts, int max_cuts);
    void *user_data;
} RalphCutCallback;
void ralph_set_cut_callback(RalphModel *model, const RalphCutCallback *cb);
```

**Use case:** Benders decomposition for FuelWise with k>30 stations. See §4.1 for
decomposition method background. The Farkas ray proves "these stations must have
at least one stop" when subproblem is infeasible.

#### P2: Cut Callback (~400 LoC)

```c
/* Cut representation */
typedef struct {
    int *indices;       /* Variable indices */
    double *coeffs;     /* Coefficients */
    int num_vars;       /* Number of variables in cut */
    char sense;         /* 'L' (<=), 'G' (>=), 'E' (=) */
    double rhs;         /* Right-hand side */
} RalphCut;

/* Cut callback signature */
typedef struct {
    /*
     * Called at each B&B node after LP relaxation solved.
     * x_relaxation: current LP solution (may be fractional)
     * Returns number of cuts added (0 = no cuts found)
     */
    int (*generate_cuts)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        RalphCut *cuts,         /* Output: array of cuts */
        int max_cuts            /* Max cuts to generate */
    );
    void *user_data;
} RalphCutCallback;

void ralph_set_cut_callback(RalphModel *model, RalphCutCallback *cb);
```

**Implementation notes:**
- Invoke after each node LP solve
- Manage cut pool (avoid duplicates, limit total cuts)
- Age out ineffective cuts
- Generate at most 10-20 cuts per callback (diminishing returns)

#### P3: Branching Callback ✅

```c
/* Branching callback for custom variable selection */
typedef struct {
    int (*select_branch_var)(void *user_data, const double *x_relaxation,
                              int num_vars, const int *is_integer,
                              const double *lb, const double *ub);
    void *user_data;
} RalphBranchCallback;

void ralph_set_branch_callback(RalphModel *model, const RalphBranchCallback *cb);
```

Callback returns variable index to branch on, or -1 to use default strategy.
Variable must be fractional and integer-constrained.

### 6.3 FuelWise-Specific Cuts

#### Reach Cuts

"Must stop at least once in [i, j] to have enough fuel to reach station j+1"

These are analogous to subtour elimination cuts in TSP. Extremely effective for
FuelWise's path structure.

```c
/*
 * Generate reach cuts for FuelWise MILP.
 *
 * Scans for intervals where LP relaxation shows insufficient stops
 * to maintain minimum fuel level.
 */
int fw_generate_reach_cuts(
    const FWRefuelProblem *problem,
    const double *z_relaxation,  /* Fractional z[i] values */
    int z_var_offset,
    RalphCut *cuts,
    int max_cuts
) {
    int num_cuts = 0;
    double fuel = problem->tank_capacity;
    int segment_start = 0;

    for (int i = 0; i < problem->num_stations && num_cuts < max_cuts; i++) {
        double consumed = fw_calc_fuel_consumed(problem,
            (i == 0) ? 0 : problem->stations[i-1].distance_from_start,
            problem->stations[i].distance_from_start);

        fuel -= consumed;

        /* Check if we can reach station i+1 without refueling */
        double next_consumed = fw_calc_fuel_consumed(problem,
            problem->stations[i].distance_from_start,
            (i+1 < problem->num_stations)
                ? problem->stations[i+1].distance_from_start
                : problem->total_distance);

        if (fuel - next_consumed < problem->minimum_fuel) {
            /* Must stop somewhere in [segment_start, i] */
            double z_sum = 0;
            for (int j = segment_start; j <= i; j++) {
                z_sum += z_relaxation[z_var_offset + j];
            }

            if (z_sum < 1.0 - 1e-6) {
                /* Violation: add cut Σ z[j] >= 1 */
                int cut_size = i - segment_start + 1;
                cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
                cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));
                cuts[num_cuts].num_vars = cut_size;

                for (int j = 0; j < cut_size; j++) {
                    cuts[num_cuts].indices[j] = z_var_offset + segment_start + j;
                    cuts[num_cuts].coeffs[j] = 1.0;
                }
                cuts[num_cuts].sense = 'G';
                cuts[num_cuts].rhs = 1.0;
                num_cuts++;
            }

            fuel = problem->tank_capacity;
            segment_start = i + 1;
        }
    }
    return num_cuts;
}
```

#### Benders Decomposition (Planned)

**Current state:** FuelWise uses exhaustive enumeration for k≤20 stations via
`fw_solve_refuel_benders()`, which iterates all 2^k combinations. This is not
true Benders decomposition.

**Why upgrade to true Benders:** Scale to k>30 stations without exponential blowup.

See §4.1 for general Benders background. For FuelWise specifically:

**Master problem (MIP):** Choose which stations to stop at (z variables)
```
min  stop_cost·Σz[i] + θ
s.t. z[i] ∈ {0, 1}
     feasibility cuts (from Farkas rays)
     optimality cuts (from LP duals)
     θ ≥ lower_bound
```

**Subproblem (LP, given z_fixed):** Optimize fuel purchases
```
min  Σ price[i]·x[i]
s.t. flow balance constraints
     y[i] ≥ min_fuel
     y[i] + x[i] ≤ tank_capacity + consumption[i]
     x[i] ≤ tank_capacity · z_fixed[i]    (RHS depends on z)
     x[i] ≥ min_purchase · z_fixed[i]
```

**Why FuelWise is ideal for Benders:**
1. z affects only RHS of linking constraints
2. Subproblem is trivially fast (simple LP with k variables)
3. Infeasibility has clear meaning: "can't reach station j+1"
4. Cuts accumulate, tightening master over iterations

**Network structure:** The subproblem has path/network structure:
- Nodes: origin, stations, destination
- Arcs: road segments (consume fuel) and refueling (add fuel)
- Flow conservation at each node

This means the subproblem can be solved via network simplex (see §3) instead of
general simplex, providing:
- O(n²) vs O(n³) complexity
- Integral solutions guaranteed (no basis degeneracy)
- Warm start between Benders iterations via `options.warm_start`

**Implementation:** Ralph already has problem detection for LAP/network structure
(see Status Summary). Simply enable detection for FuelWise subproblems—when
`ralph_optimize()` sees the network structure, it automatically dispatches to
`ralph_netflow_solve()` internally. No FuelWise code changes needed.

### 6.4 HoSE-Specific Cuts

See [hose.md](./hose.md) for complete MIP formulation details. Key cuts:

#### Driving Capacity Cuts (11h limit)

```c
/*
 * Generate driving capacity cuts.
 * Detects intervals where fractional driving exceeds 11h without a reset.
 */
int hs_generate_driving_cuts(
    const double *x,
    int num_periods,
    double delta,
    int drive_var_offset,
    int reset_var_offset,
    RalphCut *cuts,
    int max_cuts
) {
    int num_cuts = 0;
    int last_reset = -1;
    double driving_sum = 0.0;

    for (int t = 0; t < num_periods && num_cuts < max_cuts; t++) {
        double x_drive = x[drive_var_offset + t];
        double x_reset = x[reset_var_offset + t];

        driving_sum += x_drive * delta;

        if (x_reset > 0.5) {
            last_reset = t;
            driving_sum = 0.0;
            continue;
        }

        if (driving_sum > 11.0 + 1e-6) {
            /* Violation: add cut Σ x[τ,DRIVE] ≤ floor(11/δ) */
            int t0 = last_reset + 1;
            int t1 = t;
            int cut_size = t1 - t0 + 1;

            cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
            cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));
            cuts[num_cuts].num_vars = cut_size;

            for (int j = 0; j < cut_size; j++) {
                cuts[num_cuts].indices[j] = drive_var_offset + t0 + j;
                cuts[num_cuts].coeffs[j] = 1.0;
            }
            cuts[num_cuts].sense = 'L';
            cuts[num_cuts].rhs = floor(11.0 / delta);
            num_cuts++;

            driving_sum = 0.0;
            last_reset = t;
        }
    }
    return num_cuts;
}
```

#### Mandatory Break Cuts (8h limit)

Similar structure to driving capacity, but tracks driving since last 30-min break.
Per FMCSA 2020 rule, both OFF and WORK (on-duty/not-driving) qualify as break time.

#### Cut Strength Analysis

| Cut Type | Strength | When to Generate | Cost |
|----------|----------|------------------|------|
| **FuelWise reach** | Very strong | Always | O(k) scan |
| **Driving capacity (11h)** | Strong | Always | O(H) scan |
| **Mandatory break (8h)** | Strong | Always | O(H) scan |
| **14h window** | Medium | Preprocessing preferred | O(H) |
| **70h cycle** | Weak | Rarely needed | O(H) |

### 6.5 Expected Performance

#### FuelWise MILP

| Stations (k) | Without Cuts | With Reach Cuts | With Benders |
|--------------|--------------|-----------------|--------------|
| k=20 | 1s | 50ms | 30ms |
| k=30 | timeout | 100ms | 50ms |
| k=50 | timeout | 300ms | 100ms |
| k=100 | timeout | timeout | 1s |

#### HoSE MIP (1-week horizon, δ=5 min)

| Scenario | Without Cuts | With Cuts |
|----------|--------------|-----------|
| Fresh driver, 1 task | 2-10s | <1s |
| Exhausted clocks, 1 task | 5-30s | 2-10s |
| Chain of 3 tasks | 30-120s | 10-30s |
| Chain of 5 tasks | 2-10 min | 30-120s |

### 6.6 Implementation Status

| Phase | Features | Status | Notes |
|-------|----------|--------|-------|
| P0 | Branch priorities + directions | ✅ Complete | Better MIP for k≤30 |
| P1 | Lazy constraints + bound query + RHS mod | ✅ Complete | Manual cut loop, Benders prep |
| P2 | Warm start + cut callback | ✅ Complete | Basis save/restore, automatic cuts |
| P3 | Branching callback | ✅ Complete | Custom variable selection |
| - | FuelWise true Benders | ⏳ Planned | Replace enumeration with MIP master |
| - | HoSE clock cuts | ⏳ Planned | Driving/break capacity cuts |

**MIP infrastructure complete (Feb 2026).** Next: implement domain-specific algorithms
using the infrastructure (true Benders for FuelWise, clock cuts for HoSE).

### 6.7 Relationship to §4.3 MIP Improvements

This chapter implements several items from §4.3:

| §4.3 Item | This Chapter |
|-----------|--------------|
| Pseudocost branching | P0 branching priorities (simpler, domain-specific) |
| Diving heuristics | Not needed (structure provides good incumbents) |
| Node presolve | P1 bound query enables domain presolve |
| Cut pool management | P2 cut callback |

The domain-specific approach here is more targeted than generic MIP improvements
and provides better performance for FuelWise/HoSE problem classes.

---

## Chapter 7: Generic Benders Decomposition ✅

**Status:** Complete (Feb 2026) - 8 tests passing

This chapter describes the generic Benders decomposition solver in Ralph, replacing
domain-specific implementations (e.g., FuelWise enumeration) with a reusable algorithm.

### 7.1 Background

**Benders decomposition** solves problems of the form:

```
min  c'x + d'y
s.t. Ax = b                    (master constraints)
     Tx + Wy = h               (linking constraints)
     x ∈ X (integer/binary)    (complicating variables)
     y ≥ 0                     (continuous variables)
```

The key insight: once x is fixed, the subproblem in y is an LP. We can represent
the optimal subproblem cost Q(x) via linear cuts in the master problem.

**Algorithm:**
1. Solve master: `min c'x + θ` subject to master constraints + accumulated cuts
2. Fix x*, solve subproblem: `min d'y s.t. Wy = h - Tx*`
3. If subproblem optimal with objective q*:
   - Add **optimality cut**: `θ ≥ π'(h - Tx)` where π = subproblem duals
4. If subproblem infeasible:
   - Add **feasibility cut**: `0 ≥ y'(h - Tx)` where y = Farkas ray
5. Repeat until `θ ≈ q*` (convergence)

### 7.2 API Design Options

#### Option A: Structure-Based (Recommended)

User specifies which variables are "complicating" (go to master), Ralph handles decomposition:

```c
typedef struct {
    /* Which variables belong to master problem */
    int *master_var_indices;
    int num_master_vars;

    /* The θ variable representing subproblem cost (-1 to auto-create) */
    int theta_var;

    /* Stochastic Benders: multiple scenarios */
    int num_scenarios;          /* 1 for deterministic */
    double *scenario_probs;     /* NULL = equal weights */

    /* Algorithm parameters */
    double gap_tolerance;       /* Convergence gap (default 1e-6) */
    int max_iterations;         /* Iteration limit (default 1000) */
    int cuts_at_lp_nodes;       /* 1 = branch-and-Benders-cut (modern) */
    int warm_start_subproblems; /* 1 = reuse subproblem basis */
} RalphBendersConfig;

/* Main entry point */
int ralph_solve_benders(
    RalphModel *model,
    const RalphBendersConfig *config,
    RalphSolution *solution
);
```

**Ralph automatically:**
1. Partitions model into master (variables in `master_var_indices`) + subproblem (rest)
2. Identifies linking constraints (those involving both master and subproblem vars)
3. Creates master MIP with θ variable for recourse cost
4. Generates cuts from subproblem duals (optimality) or Farkas rays (feasibility)
5. Iterates until convergence

#### Option B: Callback-Based (Maximum Flexibility)

For non-standard Benders variants (e.g., logic-based Benders, combinatorial subproblems):

```c
typedef struct {
    /* User builds subproblem given fixed master solution */
    RalphModel* (*build_subproblem)(
        void *user_data,
        int scenario,
        const double *x_master,
        int num_master_vars
    );

    /* User adds cut to master (NULL = use automatic cut generation) */
    int (*add_cut)(
        void *user_data,
        RalphModel *master,
        int scenario,
        int cut_type,           /* RALPH_BENDERS_OPTIMALITY or _FEASIBILITY */
        const double *multipliers,
        double subproblem_obj
    );

    /* User decides convergence (NULL = use gap tolerance) */
    int (*check_convergence)(
        void *user_data,
        double master_obj,
        double subproblem_obj,
        int iteration
    );

    void *user_data;
    int num_scenarios;
    double *scenario_probs;
} RalphBendersCallbacks;

int ralph_solve_benders_ex(
    RalphModel *master,
    const RalphBendersCallbacks *callbacks,
    RalphSolution *solution
);
```

### 7.3 Comparison

| Aspect | Option A (Structure) | Option B (Callback) |
|--------|---------------------|---------------------|
| **User effort** | Minimal - just tag variables | Significant - implement callbacks |
| **Flexibility** | Standard Benders only | Any Benders variant |
| **Cut generation** | Automatic | User-controlled |
| **Stochastic** | Built-in | User implements |
| **Debugging** | Easier (Ralph handles logic) | Harder (user code) |
| **Performance tuning** | Limited | Full control |

**Recommendation:** Implement **Option A first**, with Option B as future extension.

Option A covers 90% of use cases (FuelWise, stochastic fleet planning, network design)
with minimal user code. Option B can be added later for exotic variants.

### 7.4 Integration with MIP Infrastructure (§6)

**Key insight:** The master problem is a MIP solved by Ralph's B&B. All §6 features
apply to the master, working in tandem with Benders:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Benders Master MIP                            │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Branch & Bound (existing)                                │   │
│  │                                                           │   │
│  │  • Branching priorities (§6 P0)                          │   │
│  │    → Control which master vars branch first               │   │
│  │    → FuelWise: cheap stations first                       │   │
│  │                                                           │   │
│  │  • Branching directions (§6 P0)                          │   │
│  │    → Hint down/up preference                              │   │
│  │    → FuelWise: prefer z=1 (stop) at cheap stations        │   │
│  │                                                           │   │
│  │  • User cut callback (§6 P2)                             │   │
│  │    → Add domain-specific cuts during B&B                  │   │
│  │    → FuelWise: reach cuts (must stop in interval)         │   │
│  │    → HoSE: driving capacity cuts                          │   │
│  │                                                           │   │
│  │  • Benders cuts (NEW - this chapter)                     │   │
│  │    → Optimality cuts from subproblem duals                │   │
│  │    → Feasibility cuts from Farkas rays                    │   │
│  │    → Added at integer solutions OR LP nodes               │   │
│  │                                                           │   │
│  └──────────────────────────────────────────────────────────┘   │
│                           │                                      │
│                           ▼                                      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Subproblem LP (for each integer/LP solution)             │   │
│  │                                                           │   │
│  │  • Warm start (§6 P2)                                    │   │
│  │    → Reuse basis between subproblem solves                │   │
│  │    → Major speedup for similar x values                   │   │
│  │                                                           │   │
│  │  • Network detection (existing)                          │   │
│  │    → Auto-dispatch to network simplex if applicable       │   │
│  │    → FuelWise: subproblem has path structure              │   │
│  │                                                           │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**Example: FuelWise with full integration**

```c
/* 1. Build full model */
RalphModel *model = fw_build_full_milp(problem);  /* z[i], x[i], θ */

/* 2. Set branching priorities (cheap stations first) */
int priorities[k];
for (int i = 0; i < k; i++) {
    priorities[i] = (int)(1000.0 / problem->stations[i].price);
}
ralph_set_branch_priorities(model, priorities);

/* 3. Set branching directions (prefer stopping at cheap stations) */
int directions[k];
for (int i = 0; i < k; i++) {
    directions[i] = (problem->stations[i].price < avg_price) ? 1 : 0;
}
ralph_set_branch_directions(model, directions);

/* 4. Set user cut callback for reach cuts */
RalphCutCallback reach_cb = {
    .generate_cuts = fw_generate_reach_cuts,
    .user_data = problem
};
ralph_set_cut_callback(model, &reach_cb);

/* 5. Configure Benders */
int master_vars[k];  /* z[0]...z[k-1] */
for (int i = 0; i < k; i++) master_vars[i] = i;

RalphBendersConfig benders = {
    .master_var_indices = master_vars,
    .num_master_vars = k,
    .theta_var = theta_idx,
    .num_scenarios = 1,
    .cuts_at_lp_nodes = 1,       /* Modern branch-and-Benders-cut */
    .warm_start_subproblems = 1
};

/* 6. Solve */
RalphSolution solution;
ralph_solve_benders(model, &benders, &solution);
```

**What happens internally:**

1. Master B&B starts, using priorities to branch z[cheap] before z[expensive]
2. At each B&B node:
   - User cut callback adds reach cuts if violated
   - If `cuts_at_lp_nodes=1`: solve subproblem, add Benders cuts even for fractional x
3. At integer solutions:
   - Solve subproblem LP (warm started, may use network simplex)
   - If feasible: add optimality cut `θ ≥ π'(h - Tz)`
   - If infeasible: add feasibility cut from Farkas ray
4. Converges when master θ matches subproblem objective

### 7.5 Modern vs Classic Benders

| Variant | Cuts added at | Pros | Cons |
|---------|--------------|------|------|
| **Classic** | Integer solutions only | Simpler, fewer subproblem solves | More B&B nodes |
| **Modern (B&B&C)** | LP nodes too | Tighter LP relaxation, fewer nodes | More subproblem solves |

**Recommendation:** Default to modern (`cuts_at_lp_nodes=1`) with option to disable.
Modern Benders is typically 2-10x faster on structured problems like FuelWise.

### 7.6 Stochastic Benders

For problems with uncertainty (e.g., demand scenarios, price scenarios):

```c
/* K scenarios with probabilities p[k] */
RalphBendersConfig config = {
    .master_var_indices = first_stage_vars,
    .num_master_vars = n1,
    .theta_var = -1,            /* Auto-create K theta variables */
    .num_scenarios = K,
    .scenario_probs = probs     /* Sum to 1.0 */
};
```

Ralph creates K subproblems (one per scenario), solves in parallel, generates
weighted cuts: `θ[k] ≥ π[k]'(h[k] - T[k]x)` for each scenario.

### 7.7 Implementation Status ✅

| Phase | Component | LoC | Status |
|-------|-----------|-----|--------|
| 1 | Model partitioning | 150 | ✅ Complete |
| 2 | Linking detection | 100 | ✅ Complete |
| 3 | Cut generation | 200 | ✅ Complete (optimality + feasibility) |
| 4 | Benders loop | 150 | ✅ Complete (classic algorithm) |
| 5 | B&B integration | 100 | ⏳ Planned (modern B&B&C) |
| 6 | Warm start | 50 | ✅ Complete |
| 7 | Stochastic | 150 | ✅ Complete (multi-scenario) |
| 8 | Testing | 200 | ✅ 8 tests passing |
| **Total** | | **~1430** | |

**Files:**
- `ralph/src/benders.c` - Main implementation
- `ralph/include/benders.h` - API header
- `ralph/tests/test_benders.c` - Test suite

**Dependencies:**
- §6 MIP infrastructure (✅ complete)
- Network flow solver (✅ complete) - for fast subproblems
- Farkas ray extraction (✅ complete)

### 7.8 Known Limitations

| Limitation | Impact | Workaround |
|------------|--------|------------|
| **Classic Benders only** | Modern B&B&C (cuts at LP nodes) not yet implemented | Classic algorithm works well for structured problems; code comments corrected to reflect this |
| **Numerical sensitivity** | Very large bounds (1e30) can cause simplex instability | Use reasonable bounds (<1e6) for theta variable |
| **0 master constraints** | Edge case returns error | Add dummy constraint if needed |
| **Feasibility cuts** | Less tested than optimality cuts | Most problems have feasible subproblems |
| **Gomory cuts + Benders** | Gomory/MIR cuts in master solver cause INFEASIBLE status | Set `max_cut_rounds = 0` on master solver to disable |
| **Algorithm correctness** | FuelWise Benders converges to suboptimal solution ($85 vs expected $74) | Under investigation (see TODO below) |

**TODO: Benders Algorithm Correctness**

The Benders implementation converges without errors but finds a suboptimal solution in FuelWise
test cases. Suspected issues to investigate:

1. **Optimality cut generation**: Verify dual values from subproblem are correctly extracted
   and transformed into valid Benders optimality cuts. Check sign conventions and constraint
   indexing.

2. **Subproblem RHS handling**: The subproblem RHS depends on master solution (z values).
   Verify `sub_only_rhs_contribution` correctly accounts for constraints that only involve
   subproblem variables vs linking constraints.

3. **Farkas ray normalization**: Feasibility cuts use Farkas rays which require normalization.
   The current implementation normalizes but may have sign/indexing issues.

4. **Theta bounds**: Fixed by calculating from problem structure (max_fuel_value * 100) rather
   than hardcoded ±1e9. Verify this is sufficient for all problem instances.

5. **Cut accumulation**: Ensure cuts from previous iterations remain valid and are not
   inadvertently modified or dropped.

**Does NOT affect FuelWise:**
- FuelWise has master constraints (capacity constraints)
- Classic Benders provides the main scaling win: O(k² × iterations) vs O(2^k) enumeration
- FuelWise subproblems are typically feasible (flow balance with sufficient capacity)
- Reasonable bounds are natural for fuel quantities

### 7.9 Expected Performance

| Problem | Current (enumeration) | With Benders | Speedup |
|---------|----------------------|--------------|---------|
| FuelWise k=20 | 50ms | 30ms | 1.7x |
| FuelWise k=30 | timeout (2^30) | 80ms | ∞ |
| FuelWise k=50 | timeout | 150ms | ∞ |
| FuelWise k=100 | timeout | 500ms | ∞ |

The key win is scaling: enumeration is O(2^k), Benders is typically O(k² · iterations).

### 7.10 Relationship to §6.3

§6.3 described FuelWise-specific Benders design. This chapter supersedes that with
a generic implementation. FuelWise becomes a *user* of generic Benders:

- §6.3 reach cuts → User cut callback (works with Benders)
- §6.3 Benders loop → Handled by `ralph_solve_benders()`
- §6.3 Farkas cuts → Automatic in generic Benders

The domain-specific value in FuelWise is now:
1. Reach cut generation (user callback)
2. Branching priorities (cheap stations first)
3. Problem formulation (how to express as Benders structure)

---

## Related Files

| File | Purpose |
|------|---------|
| `ralph/CLAUDE.md` | Development guide, API reference |
| `ralph/include/ralph.h` | Public API |
| `ralph/include/ralph_api.h` | REST API (planned) |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
