# Ralph Simplex Architecture Review

**Date:** 2026-02-16
**Baseline:** `f58b421` (universal two-phase + redundant row presolve)
**Branch:** `feature/724da701-ralph-mip-infrastructure`

---

## 1. Findings Summary

### What's Good

- **No Big-M anywhere.** Universal two-phase simplex (`num_artificial > 0` trigger) eliminated
  objective contamination. kb2/recipe/scorpion accuracy fixed.
- **Clean method dispatch.** `simplex_solve()` tries dual first (method=2), Ax=b sanity check,
  verify_solution gate, IMPRECISE downgrade triggers primal fallback. Defense-in-depth.
- **6-metric post-solve verification.** Primal feasibility, bounds, dual feasibility,
  complementary slackness, Kahan-summed objective, condition estimate. OPTIMAL→IMPRECISE downgrade.
- **Dual simplex is independent.** Separate tableau creation (`tableau_create_ex(..., 0, 1)`),
  no artificials, `make_dual_feasible` + `dual_phase1` + `dual_simplex_solve_v2`. Clean.
- **MIP warm-start works.** Bound update → `dual_v2_clear_perturbation` → recompute →
  500-pivot v2 → cold-start primal fallback. Correct and testable.
- **Scaling.** Geometric mean + equilibrium rounds, virtual accumulation, proper unscale.
- **Anti-cycling stack.** Bland's rule fallback, proactive perturbation, excluded-entering TTL,
  refactor intervals. Multiple safety nets.
- **Farkas certificate.** `extract_farkas_ray_dual` with y'b < 0 validation.

### What's Risky

- **Phase 1→2 transition.** beaconfd: 57/140 stuck artificials → near-singular Phase 2 basis →
  obj=-7.57e31. The eviction loop (`simplex_transition_phase2`) tries BTRAN + single best-pivot
  per artificial. Pivot tolerance 0.1 is too high for rank-deficient problems; needs relaxed
  tolerance + auxiliary pivots for rows where structural coefficients are all small.
- **Primal unboundedness detection.** `theta_max >= RALPH_INFINITY/2` in Harris ratio test
  returns -1 immediately. No refactorize-and-retry. lotfi fails here: stale LU produces
  spurious `theta=inf` that a fresh factorization would fix.
- **-O3 code layout sensitivity.** Changing constants in hot simplex paths (perturbation
  threshold, weight resets) causes instruction cache/alignment shifts that break unrelated
  problems (brandy). Any Phase 1 fix must be tested with full NETLIB under -O3.
- **Devex weight continuity.** Weights trained during Phase 1 encode Phase 1's objective
  landscape. They are NOT meaningful for Phase 2's original objective, yet resetting them
  destabilizes brandy catastrophically. The weights are "accidentally useful" — they provide
  reasonable column-norm information even though their optimization signal is wrong.

### What's Missing

| Gap | Impact | Effort |
|-----|--------|--------|
| Phase 1→2 stuck-artificial improvement | Fix beaconfd (22/22 NETLIB) | ~100 LoC |
| Refactorize-before-unbounded safety net | Fix lotfi (22/22 NETLIB) | ~30 LoC |
| Lightweight MIP warm-start (skip perturb/unshift) | 10-100x MIP speedup | ~200 LoC |
| Cover cuts | Close milp15/milp50 obj gap | ~400 LoC |
| Cut pool management (prune inactive) | 3-5x milp100+ speed | ~300 LoC |

---

## 2. Current Architecture Map

### Method Dispatch (`simplex_solve`, line 4504)

```
simplex_solve(solver)
  ├─ method=0 → primal path (tableau_create_ex + crash + Phase 1 + Phase 2)
  ├─ method=1 → dual only (dual_simplex_solve_from_scratch_v2, error on failure)
  └─ method=2 (default) → dual first, with safety gates:
       ├─ dual_simplex_solve_from_scratch_v2()
       ├─ Gate 1: Ax=b sanity check (row-wise, tol=1e-4)
       ├─ Gate 2: non-OPTIMAL status → reject, fall back
       ├─ Gate 3: verify_solution → IMPRECISE → fall back
       └─ Fallback: destroy dual tableau, recreate primal, full solve
```

### Primal Path

```
tableau_create_ex(model, force_two_phase=0, dual_mode=0)
  └─ num_artificial > 0 → use_two_phase = 1, artificial cost = 1.0
crash_triangular() [method=0 only, saves+restores on failure]
tableau_refactorize()
  └─ singular crash basis → restore original
simplex_phase1() [if use_two_phase]
  ├─ Proactive perturbation if equalities > 90%
  ├─ Pricing: Devex (default), with Bland fallback on cycling
  ├─ Anti-cycling: degenerate counter, excluded-entering, refactor interval
  └─ Termination: art_sum < FEAS_TOL → feasible, art_sum > 0 → INFEASIBLE
simplex_transition_phase2()
  ├─ Switch c_ext to c_original
  ├─ Evict stuck artificials via BTRAN + best-pivot search
  ├─ Mark un-evictable rows as redundant
  ├─ Set stuck artificial costs to 0
  ├─ Refactorize (with repair fallback)
  └─ Recompute RC + solution
simplex_phase2()
  ├─ Force early refactorize for two-phase
  ├─ Same pricing/anti-cycling as Phase 1
  └─ Termination: no improving RC → OPTIMAL, theta=inf → UNBOUNDED
```

### Dual Path

```
dual_simplex_solve_from_scratch_v2(solver)
  └─ tableau_create_ex(model, 0, dual_mode=1)
       └─ No artificials: ≤ → slack(+1), ≥ → surplus(-1), = → fixed slack
make_dual_feasible(tab)
  └─ Flip non-basics to bound satisfying rc sign condition
dual_phase1(solver) [if residual infeasibilities after flips]
  ├─ Auxiliary objective: c[j] = ±1 for infeasible non-basics
  ├─ Perturbation for cycling prevention
  ├─ DSE leaving selection (infeas²/weight)
  ├─ Check original dual feasibility every 5 iters
  └─ 200*m iteration limit
dual_simplex_solve_v2(solver)
  ├─ Perturbation + DSE (conditional init)
  ├─ Bound flipping (P5) — disabled with perturbation
  └─ Unshift cleanup pivots after removing perturbation
```

### MIP Warm-Start Path

```
solve_node_lp(solver, node)
  ├─ If tab exists and num_artificial == 0:
  │    ├─ Update lb_ext/ub_ext from node
  │    ├─ Push non-basics to new bounds
  │    ├─ dual_v2_clear_perturbation()
  │    ├─ Recompute solution + RC
  │    ├─ dual_simplex_solve_v2() with 500-pivot budget
  │    └─ Accept OPTIMAL/INFEASIBLE/OBJ_LIMIT
  └─ Cold-start fallback: destroy tableau, simplex_solve(method=0)
```

### Tolerances (lp.h, lines 16-33)

| Constant | Value | Used For |
|----------|-------|----------|
| `RALPH_FEAS_TOL` | 1e-6 | Primal feasibility, Harris ratio slack |
| `RALPH_OPT_TOL` | 1e-6 | Reduced cost optimality |
| `RALPH_PIVOT_TOL` | 1e-6 | Minimum pivot element |
| `RALPH_ZERO_TOL` | 1e-12 | Absolute zero |
| `RALPH_INT_TOL` | 1e-5 | Integer feasibility (MIP) |
| `RALPH_FORCE_REFACTOR_PIVOT_TOL` | 1e-4 | Force refactorize on small pivot |
| `RALPH_LU_GROWTH_REFACTOR_THRESHOLD` | 1e8 | LU element growth trigger |

### Scaling

- Geometric mean rounds (configurable, default=solver->scaling)
- Equilibrium rounds (20 rounds if geo > 1, convergence check at 10% deviation)
- Virtual accumulation: row_scale[], col_scale[] built without modifying A
- Applied once at end: A, b, c, lb, ub all scaled
- Unscaled in `unscale_solution()` after solve

---

## 3. Feasibility Strategy (No Big-M)

### Design Decision

**Universal two-phase simplex for all problems with artificial variables.**

- Trigger: `num_artificial > 0` (line 883)
- Phase 1 objective: structural vars cost=0, artificial vars cost=1.0
- Phase 2: original objective restored in `simplex_transition_phase2`
- Big-M constant (`RALPH_BIG_M`) fully removed from codebase

### Verified Clean

- No `BIG_M` references in any `.c` or `.h` file
- `artificial_cost = 1.0` unconditionally (line 962)
- Post-solve objective computed from structural variables only (Phase 2 handles this)

### Remaining Risk

Phase 1→2 transition fragility for problems with many stuck artificials (see P0 action items).

---

## 4. Dual Simplex Requirements

### A. Bound Flipping (make_dual_feasible)

`make_dual_feasible()` (dual_simplex.c:721-766):
- Iterates all non-basic variables
- At LB with rc < -OPT_TOL: flip to UB (if finite)
- At UB with rc > OPT_TOL: flip to LB (if finite)
- Free variables with rc ≠ 0: cannot fix by flipping, deferred to `dual_phase1`
- Returns count of changes made

**Assessment:** Correct. Handles bounded, unbounded, and free variables. Free-variable
residual infeasibility is properly delegated to Phase 1.

### B. Dual Phase 1

`dual_phase1()` (dual_simplex.c:1212-1399):
- Constructs auxiliary objective: c[j] = ±1 for dual-infeasible non-basics, 0 otherwise
- Saves and restores original objective
- Uses bound perturbation for cycling prevention
- DSE-weighted leaving selection (infeas²/weight) when available
- Checks original dual feasibility every 5 iterations (early termination)
- 200*m iteration limit
- Post-Phase1: calls `make_dual_feasible()` again as cleanup

**Assessment:** Sound design. The auxiliary objective correctly drives dual-infeasible
variables toward feasibility. The periodic original-feasibility check is a good optimization.
One concern: after restoring original objective, a final `make_dual_feasible` call may flip
variables that were correctly positioned by Phase 1 pivots — but this is low-risk since
the basis (which Phase 1 changed) determines the dual values that drive the flips.

### C. Ranged Rows

Ralph does not support ranged rows (L ≤ Ax ≤ U where both L and U are finite and L < U).
All constraints are normalized to `<=`, `>=`, or `=` with non-negative RHS. This is adequate
for FuelWise and most standard MPS problems. NETLIB problems that use RANGES sections are
handled by the MPS reader converting them to two constraints.

### D. Anti-Cycling (Dual)

Dual simplex uses:
1. **Bound perturbation** (dual_simplex.c:778-816): Pseudo-random, backup-only-on-first-call.
   Applied in `dual_phase1` and `dual_simplex_solve_v2`.
2. **Bound flipping (P5)**: Disabled when perturbation is active (incompatible — flip magnitude
   corrupted by perturbed bounds).
3. **Refactorization**: Periodic (every 50 iters in Phase 1, `lu_needs_refactorization` in Phase 2).

**Assessment:** Adequate for typical problems. Missing: dual Bland's rule fallback (primal
has one). Not observed as a problem in practice — dual perturbation is more effective than
primal perturbation for preventing cycling.

### E. Numerical Safeguards

1. **LU refactorization triggers**: max updates, element growth > 1e8, condition-based,
   spike pool full (lu.c:1834-1866)
2. **Singular pivot regularization**: LU factorization detects near-zero pivots and
   regularizes for redundant rows (lu.c:449-512)
3. **Verify solution**: 6-metric check, OPTIMAL→IMPRECISE downgrade (simplex.c:420-555)
4. **Ax=b sanity check**: Row-wise dot product verification in method=2 auto mode before
   committing dual result (simplex.c:4694-4707, tol=1e-4)
5. **Perturbation backup-only-first**: Prevents re-perturbation from corrupting backup bounds
   (fixed bug: brandy 0.18% obj error)
6. **Crash basis revert**: If crash produces singular or infeasible basis, restore original
   (simplex.c:4612-4668)
7. **Basis repair**: `repair_singular_basis()` called when refactorization fails during
   Phase 2 transition

**Assessment:** Good layered defense. The verify_solution gate in method=2 is particularly
valuable — it catches silently wrong dual solutions. Gap: no refactorize-before-declaring-
unbounded safety net in primal (see P0.2).

---

## 5. How Dual Simplex Changes Phase I

### Primal Phase 1 (simplex_phase1)

- Minimizes sum of artificial variable values
- Pivots reduce art_sum toward 0
- Terminates: art_sum < FEAS_TOL (feasible) or no improving variable (infeasible)
- Proactive perturbation when equalities > 90% of constraints

### Dual Phase 1 (dual_phase1)

- Minimizes count of dual infeasibilities via auxiliary objective
- Pivots change basis to satisfy dual feasibility conditions
- Terminates: all rc satisfy bound conditions (feasible) or iteration limit
- Separate from primal Phase 1 — they solve different problems

### Key Difference

Primal Phase 1 answers "does a feasible point exist?" (certificates: artificial_sum = 0 or > 0).
Dual Phase 1 answers "can we start dual Phase 2 from a dual-feasible basis?" (then dual Phase 2
finds primal feasibility via dual pivots).

### Interaction in method=2

```
Try dual:
  make_dual_feasible → dual_phase1 → dual_solve_v2
  If dual says INFEASIBLE → DON'T TRUST (auto mode rejects non-OPTIMAL)
  If dual says OPTIMAL → verify → accept or fall back

Fall back to primal:
  Phase 1 (primal) → Phase 2 (primal)
  Primal Phase 1 is the authoritative feasibility oracle
```

This is correct: dual infeasibility detection is unreliable for Ralph's implementation
(comment at simplex.c:4714-4715), so auto mode only trusts OPTIMAL from dual.

---

## 6. Prioritized Action List

### P0: Critical (fix NETLIB failures)

**P0.1: Dense LU col_perm reset** — DONE

Root cause: `lu_factorize_dense()` did not reset `col_perm`/`col_perm_inv` to identity.
When `lu_factorize_sparse_efficient` failed (identity column placement conflict with
regularized redundant rows) and fell back to dense, `col_perm` retained the non-trivial
column ordering from a previous successful sparse factorization. This caused `lu_solve`
to apply the stale permutation → FTRAN returned all zeros → spurious UNBOUNDED.

Fix: 3 lines at the start of `lu_factorize_dense()` in `lu.c`:
```c
lu->col_perm[i] = i;
lu->col_perm_inv[i] = i;
```

Also includes A_ext row zeroing in `simplex_transition_phase2` for problems with stuck
artificials (redundant rows). This makes the basis well-conditioned for the dense LU fallback.

Result: lotfi PASS (err=8.7e-10), brandy stable (152ms), zero regressions.

Remaining: beaconfd still ERROR (Phase 1 cycling at 81% equalities). Lowering the
perturbation threshold from 90% to always-on fixes beaconfd's Phase 1 but causes
brandy timeout via -O3 code layout sensitivity. Needs reactive perturbation approach.

**P0.2: Refactorize-before-unbounded in primal** — DONE

Fix: In `simplex_phase2`, when ratio test returns UNBOUNDED, refactorize and retry once.

### P1: High Priority (MIP performance)

**P1.1: Lightweight MIP warm-start mode** (~200 LoC)

Add `dual_simplex_solve_v2_lightweight()` or a flag to v2 that:
- Skips bound perturbation (Phase 2 only, budget < 500)
- Skips unshift cleanup
- Uses approximate DSE (weights=1.0) instead of exact
- No Farkas certificate extraction

This recovers most of `dual_reopt`'s per-node speed while keeping the clean architecture.

**P1.2: Cover cuts** (~400 LoC)

FuelWise MILPs have knapsack structure. Cover cuts would close the milp15/milp50 objective
gap (currently 124.5% and 36.5% vs GLPK).

**P1.3: Cut pool management** (~300 LoC)

Currently all cuts stay in the LP forever. At milp100+, working LP grows 3-5x. Prune cuts
with zero dual value for > 5 rounds.

### P2: Medium Priority (robustness polish)

**P2.1: Dual Bland's rule fallback**

Add dual-side Bland's rule for entering variable selection when cycling is detected in
dual Phase 2. Currently only perturbation is used.

**P2.2: Phase 1 perturbation threshold investigation**

The 90% threshold leaves beaconfd (81% equalities) unperturbed. A lower threshold would
help, but -O3 sensitivity prevents simple constant changes. Needs investigation of
alternative approaches (e.g., reactive perturbation triggered by cycling counter in Phase 1).

**P2.3: Exact primal steepest edge**

Replace Devex approximation with exact SE for primal simplex. 10-30% fewer iterations on
degenerate problems. Medium effort (~200 LoC) but requires careful weight maintenance.

---

## 7. Design Decisions Record

| Decision | Rationale | Status |
|----------|-----------|--------|
| **No Big-M** | Eliminates objective contamination (kb2: 19% → 0% error) | Implemented, verified clean |
| **Dual simplex first (method=2)** | Dual is better for bounded problems; auto mode safely falls back | Implemented, 3 safety gates |
| **Primal Phase 1 as authoritative feasibility oracle** | Dual infeasibility detection unreliable; primal Phase 1 is definitive | Correct design |
| **6-metric verify_solution** | Catches silently wrong solutions; enables IMPRECISE downgrade | Implemented |
| **Conditional DSE init** | Avoid O(m²) cost on every MIP node; weights persist across nodes | Implemented (`f80487e`) |
| **Perturbation backup-only-first** | Prevents re-perturbation from corrupting original bounds | Fixed (`f80487e`) |
| **500-pivot MIP warm-start budget** | Prevents catastrophic per-node cost while allowing most nodes to converge | Implemented |
| **Stuck artificials → redundant rows** | Rank-deficient constraints marked, LU regularizes their pivots | Implemented, needs improvement (P0.1) |
| **Do NOT reset Devex weights at Phase transition** | Resetting destabilizes brandy catastrophically; current weights provide useful column-norm info | Decision recorded, not ideal but necessary |
| **Do NOT lower perturbation threshold below 90%** | -O3 code layout sensitivity causes brandy regression at any other threshold value | Decision recorded, needs alternative approach (P2.2) |

---

## 8. Testing Plan

### Regression Testing (before any change)

1. `make -C ralph clean && make -C ralph test` — 359 LP/MIP tests
2. `make -C ralph test-lap` — 358 LAP tests
3. `make -C ralph test-netflow` — 153 Network Flow tests
4. `make -C ralph test-detect` — 194 detection tests
5. `./ralph-benchmark --test fast` — NETLIB tier 0-1 (verify brandy passes as canary)

### After P0.1 (stuck-artificial fix)

- beaconfd must reach OPTIMAL with obj error < 1e-4 vs reference 33592.4858072
- All 20 currently-passing NETLIB problems must still pass
- brandy must complete in < 300ms (was 150ms, allow 2x margin)
- `make test` must pass (watch for two-phase regressions)

### After P0.2 (unbounded retry)

- lotfi must reach OPTIMAL with obj error < 1e-4 vs reference -25.2647
- All other NETLIB results unchanged
- Add explicit test: load lotfi, method=0, verify OPTIMAL

### After P1.1 (lightweight warm-start)

- FuelWise milp benchmarks (seeds 42, 123, 456, 789, 1337):
  - milp15: < 50ms, obj gap < 5%
  - milp50: < 2s, obj gap < 5%
  - milp100: < 60s, obj gap < 2%
- All LP tests still pass (warm-start changes must not affect LP path)
- `make test` — all MIP tests pass

### Canary Tests (run on EVERY change)

- brandy: must pass in < 300ms (most sensitive to simplex changes)
- kb2: must have obj error < 1e-4 (sensitive to Big-M regression)
- recipe: must have obj error < 1e-4 (sensitive to Big-M regression)
