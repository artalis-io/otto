# Ralph Solver Roadmap

Development roadmap for Ralph LP/MIP solver. For completed work history, see
`docs/archive/ralph-roadmap-pre-r4.md`.

## Current State (Mar 2026)

Ralph is a complete LP/MIP solver suite in ~25K lines of C with zero external
dependencies: primal simplex, dual simplex, LAP (JVC), network simplex, branch-
and-bound MIP, problem structure detection, and Benders decomposition. Compiles
to WASM for browser demos.

### Architecture (post-R3/R4)

| File | Lines | Purpose |
|------|-------|---------|
| `simplex.c` | 4,255 | Core: tableau, pivot, phase1/phase2 orchestrators, solve entry |
| `simplex_phase1_zones.c` | 2,447 | 6 Phase 1 crisis zone handlers |
| `simplex_phase2_zones.c` | 822 | 6 Phase 2 zone handlers |
| `simplex_refactor_schedule.c` | 804 | Soft LU, periodic cost, reinvert controller |
| `simplex_phase1_stabilize.c` | 748 | Failed-stabilize retry + recompute helpers |
| `simplex_phase1_recovery.c` | 641 | Recovery state + progress ops |
| `simplex_pricing.c` | 582 | Pricing strategies (Dantzig, Bland, Devex, SE, heap) |
| `simplex_phase1_decision.c` | 497 | No-pivot ladder, relax plans, escape gates |
| `simplex_ratio.c` | 487 | Harris ratio test + bound flipping |
| `simplex_scaling.c` | 462 | Geometric mean scaling + solution verification |
| `simplex_phase1_trace.c` | 328 | Trace recording + stagnation detection |
| `simplex_perturb.c` | 198 | Bound perturbation |
| `simplex_crash.c` | 174 | Triangular crash basis (Maros LTSF) |
| `dual_simplex.c` | 3,301 | Dual simplex (DSE, Harris, Phase 1 rescue, unshift) |
| `lu.c` + `lu_sparse.c` + `lu_supernode.c` + `lu_update_backend.c` | 10,036 | LU factorization (Markowitz, FT/BG/GR update) |

### NETLIB Status

Full gate passes (84 problems): 0 objective mismatches, 0 invalid solutions.
5 timeouts on medium-large problems (fit2p, pilot, pilot.ja, wood1p, pilot.we).

### Completed Milestones

| Milestone | PRs | Date |
|-----------|-----|------|
| R0: Type system (SimplexPhase enum, LUFailureReason) | pre-R3 | Feb 2026 |
| R1: File extraction (pricing, ratio, perturbation) | pre-R3 | Feb 2026 |
| R3.1-R3.4: Recovery state + helper extraction | #29-#31 | Mar 2026 |
| R3.5: Escalation ladder unification | #32-#33 | Mar 2026 |
| R3.6: Failed-stabilize + recompute extraction | #34 | Mar 2026 |
| R3.7: Refactor scheduling extraction | #35 | Mar 2026 |
| R3.8: Phase 1 decision extraction | #36 | Mar 2026 |
| R3.9: Trace, scaling, crash extraction | #37 | Mar 2026 |
| R4.1: Phase 2 zone extraction | #38 | Mar 2026 |
| Reinversion controller (shadow + governed) | pre-R3 | Feb 2026 |
| LP presolve (12 techniques, probing) | pre-R3 | Feb 2026 |
| Dual simplex v2 (DSE, Harris, bound flip, unshift) | pre-R3 | Feb 2026 |
| LAP solver (JVC, k-best, bottleneck, priority, cardinality, qualification) | pre-R3 | Feb 2026 |
| Network simplex (MCNF, warm start, bottleneck, flow decomposition) | pre-R3 | Feb 2026 |
| Benders decomposition (8 tests) | #9 | Feb 2026 |
| MIP infrastructure (branching, cuts, warm start, callbacks) | pre-R3 | Feb 2026 |
| Supernodal LU factorization | pre-R3 | Feb 2026 |
| LP performance (B1-B7 hotspots, Devex, Markowitz, row-form RC) | pre-R3 | Feb 2026 |
| HTTP server hardening (rate limiting, work queue, stats) | pre-R3 | Mar 2026 |

---

## Active: Numerical Stability Improvements

The primal Phase 1 has 79 state variables and 15+ recovery mechanisms — more crisis
machinery than typical production solvers. Most crisis recovery exists because the
LU isn't stable enough. Fixing the LU reduces the need for crisis recovery.

### N1: Adaptive LU Threshold Pivoting (~20 lines, Low Risk)

**Problem:** Markowitz threshold is fixed at 0.1. When the basis is ill-conditioned,
this allows small pivots that amplify numerical error in FTRAN/BTRAN, causing
direction norms to blow up (triggering the entire direction stabilization crisis
machinery).

**Fix:** Adaptive threshold: start at 0.1, tighten to 0.01 when `cond_estimate > 1e6`
or `growth_factor > 100`. Both metrics are already tracked in the LU. This is what
HiGHS does.

**Files:** `lu_sparse.c` (`lu_factorize_markowitz`), ~20 lines.

**Expected effect:** Fewer direction blowups → fewer direction stabilization refactors
→ less crisis machinery activation.

**Gate:** NETLIB full gate PASS, no new timeouts.

### N2: LU Update Quality Tracking (~15 lines, Low Risk)

**Problem:** Refactorization schedule is time/iteration-based. No monitoring of
numerical quality between refactorizations. `growth_factor` and `cond_estimate`
are computed at factorization time but not tracked as FT updates accumulate.

**Fix:** After each FT update, compute `max(|spike_diag|) / min(|spike_diag|)` as
a cheap O(1) update-quality signal. Force refactorization when ratio exceeds 1e8.
Spike diagonals already stored in `ft_spike_diag[]`.

**Files:** `lu.c` or `lu_update_backend.c`, ~15 lines.

**Expected effect:** Catches basis degradation before direction norms blow up.

**Gate:** NETLIB full gate PASS.

### N3: Crisis Telemetry Audit (~100 lines script, No Risk)

**Problem:** 392 Phase 1 telemetry counters, 15+ mechanisms. Unknown which actually
fire on real workloads vs. being dead weight.

**Fix:** Run NETLIB with full telemetry. For each mechanism, count how many problems
trigger it. Expect 5-6 core mechanisms fire on 90%+ of problems; 8-9 fire on <5%.

**Deliverable:** Script + report. No code changes. Data drives N4.

### N4: Crisis Recovery Profiling (~200 lines, Medium Risk)

**Depends on:** N3 telemetry data.

**Fix:** Add `SimplexRecoveryProfile` enum (CONSERVATIVE, STANDARD, AGGRESSIVE)
that gates which adaptive/edge-case mechanisms are active:
- **Core** (always active): Bland's, perturbation, refactorization, pricing switch
- **Adaptive** (STANDARD+): direction stabilization, no-pivot ladder
- **Edge-case** (AGGRESSIVE only): shadow guard, window pressure, force extreme

Default to STANDARD. This doesn't remove mechanisms — just adds gating.

After profiling, prune mechanisms that fire on 0 NETLIB + 0 FuelWise/Surge problems.

**Gate:** NETLIB full gate PASS on all profiles.

### Priority Order

| # | Item | Lines | Risk | Impact |
|---|------|-------|------|--------|
| 1 | N1: Adaptive LU threshold | ~20 | Low | High — root cause reduction |
| 2 | N2: LU update quality | ~15 | Low | Medium — early detection |
| 3 | N3: Telemetry audit | ~100 (script) | None | High — data for pruning |
| 4 | N4: Crisis profiling | ~200 | Medium | High — coherent framework |

Items 1-3 are ~1 week. Item 4 depends on 3's data.

---

## Active: LP Presolve Enhancement

Ralph has basic presolve (12 techniques, probing with implication propagation).
Additional low-hanging fruit:

### P1: Bound Tightening from Objective (~100 lines)

When a feasible incumbent is known (MIP), tighten variable bounds using
`c_j * x_j >= incumbent - (obj without j)`. Significant for MIP re-solves.

### P2: Dominated Column Removal (~150 lines)

Remove columns where another column is strictly better in every constraint
and objective. Rare in general LP, but common in structured problems.

---

## Active: MIP Improvements

Root LP relaxation matches GLPK. The remaining gap is in tree search quality.

### M1: Cover Cuts (~400 lines, High Impact)

FuelWise MILPs have knapsack-like constraints (tank capacity, fuel balance).
Cover cuts are the standard technique — find minimal covers, lift coefficients.
Expected: milp15 gap from ~25% to <5%.

### M2: Node-Level Cut Generation (~200 lines, High Impact)

Currently cuts only at root. Add cut rounds at promising B&B nodes (depth < 10,
fractional solution with tight gap). Expected: tighter per-node bounds.

### M3: Cut Pool Management (~300 lines, High Impact)

Efficacy-based purging after N nodes. Age-out cuts not tight for 50+ nodes.
Cap total cuts at 2x original constraints. Expected: milp100+ speed 2-3x.

### M4: Feasibility Pump (~300 lines, Medium Impact)

Find incumbents before tree search via LP/rounding alternation. Complements
RINS (which needs an existing incumbent).

### M5: FuelWise True Benders (Planned)

Replace enumeration with MIP master using the Benders infrastructure from Ch. 7.

### M6: HoSE Clock Cuts (Planned)

Driving capacity cuts (11h limit) and mandatory break cuts (8h limit) for
Hours of Service optimization.

---

## Active: Dual Simplex Improvements

### D1: Leaving Variable Heap (~100 lines, Low Risk)

**Problem:** Linear scan O(m) per iteration for most-infeasible selection.

**Fix:** Maintain heap of infeasible basic variables, updated incrementally after
each pivot. Primal already has `heap[]`/`heap_pos[]` infrastructure.

### D2: Dual Devex Pricing (Planned)

Add approximate steepest edge pricing for the dual that's cheaper than
exact DSE but better than most-infeasible.

---

## Planned: REST API & WASM Demo (Chapter 5)

Port 8084, `ralph_api_*` prefix. LP/MPS in JSON, 5s timeout.
Limits: 100 vars/constraints (LP), 50 (MIP) — toy problems for WASM demo.

**Endpoints:**
```
POST /api/v1/solve      - Solve LP/MIP from JSON-embedded problem
GET  /api/v1/health     - Health check
GET  /api/v1/formats    - List supported input formats
```

**File structure:**
```
ralph/include/ralph_api.h      # Transport-agnostic API handler
ralph/src/ralph_api.c          # API handler implementation
ralph/src/ralph_parse_lp.c     # LP format parser
ralph/api/src/main.c           # Mongoose wrapper (port 8084)
ralph/wasm/ralph_wasm_api.c    # WASM wrapper
```

**Effort:** ~6-7 days.

---

## Planned: External Solver Backends

**HiGHS Integration** — Open-source LP/MIP fallback for hard problems.
Interface: `ralph_set_backend(RALPH_BACKEND_HIGHS)`.

**GLPK Integration** — GPL-licensed reference solver for benchmarking.
Interface: `ralph_set_backend(RALPH_BACKEND_GLPK)`.

---

## Planned: Architectural Cleanup (R5)

Remaining items from the R0-R4 consolidation arc:

### R5.1: Remove Dead Constants

After R3/R4, ~100 crisis-specific `#define` constants may be dead code. Compile
with `-Wunused-macros` to catch stragglers.

### R5.2: Struct Sub-Structuring

Split `SimplexTableau` (~60 fields) into named sub-structs:
```c
typedef struct {
    SimplexPricingState pricing;
    SimplexBasisState basis;
    SimplexSolutionState solution;
} SimplexTableau;
```

### R5.3: R2 Parameter Promotion (Deferred)

Replace compile-time `#define` constants with runtime parameters via
`LPRefactorPolicyConfig` struct. ~192 constants → ~50 compile-time + ~100 runtime.
Currently deferred — the constants work and the crisis profiling (N4) should
identify which ones actually need runtime tunability.

---

## Technical Reference

For deep-dive documentation, see [docs/internals/](../internals/):
- [ralph-architecture.md](../internals/ralph-architecture.md) - Solver architecture
- [lu-factorization.md](../internals/lu-factorization.md) - LU decomposition details
- [simplex.md](../internals/simplex.md) - Revised simplex implementation

## Related Files

| File | Purpose |
|------|---------|
| `ralph/include/ralph_lp.h` | LP public API |
| `ralph/include/ralph_mip.h` | MIP public API |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
| `ralph/include/detect.h` | Problem structure detection |
| `docs/archive/ralph-roadmap-pre-r4.md` | Full historical roadmap (3,977 lines) |
