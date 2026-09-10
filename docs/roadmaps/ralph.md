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

**Honest caveat (2026-09-10):** that "84/84" is the Ralph + external-GLPK-fallback
*system*, not native Ralph. The `ralph-benchmark` tool routes 18 numerically-hard
problems to the external GLPK backend. Native-only: Ralph solves ~66/84 correctly and
competitively (iteration ratio geomean 0.97 / median 1.17 vs GLPK; ~1.14× wall on
native GLPK≥50ms problems), but the hard tail is not yet native-viable — see the
Hard-Tail Closure section below and `ralph/benchmarks/netlib_perf_baseline.{json,md}`
(per-problem `solve_path`). Full frank comparison: `docs/roadmaps/ralph-vs-glop.md`.

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

### N1: Markowitz Shadow Retry for Ill-Conditioned Factorizations

**Problem:** Markowitz pivot selection is extremely sensitive. Naive adaptive
threshold/search changes (tightening from 0.1→0.05, widening search 3→12)
regressed wood1p, forplan, fit2p on NETLIB. Any parameter change alters the
factorization path, cascading through the simplex iteration.

**Approach:** Post-factorization quality gate + shadow retry. Keep the existing
factorization untouched, then conditionally re-factorize with tighter parameters
only when the result is bad. Zero risk on passing problems — gate never fires
on well-conditioned bases.

1. Markowitz runs with existing parameters (threshold=0.1, max_search=3)
2. Check `cond_estimate > 1e8` after factorization (quality gate)
3. If gate triggers: re-run Markowitz with tighter params (threshold=0.3,
   max_search=6) into shadow workspace (reuse `dense_work`/`A_struct` buffer)
4. If shadow `cond < 0.1 × primary cond` (10x improvement): accept shadow
5. Otherwise: keep primary, discard shadow

**Phases:**

| Phase | What | Effort | Risk |
|-------|------|--------|------|
| A | Quality telemetry — track `diag_ratio` after Markowitz | 1-2 hrs | None |
| B | Shadow retry — quality gate + shadow Markowitz + accept/reject | 4-6 hrs | Low |
| C | Validation — run 5 timeout problems, measure crisis reduction | 2-3 hrs | None |
| D | Runtime toggle — default OFF, enable after validation | 1 hr | None |
| E | Profile graduation — promote shadow to 3rd Markowitz profile | Future | Low |

**Files:** `lu_sparse.c` (lu_numeric_factorize ~line 3899), `lp.h` (LUFactorization
struct fields).

**Gate:** NETLIB identical on passing problems (gate never fires). Timeout problems
measured separately.

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

| # | Item | Effort | Risk | Impact |
|---|------|--------|------|--------|
| 1 | N1-A: Markowitz quality telemetry | 1-2 hrs | None | Enables N1-B |
| 2 | N3: Crisis telemetry audit | ~100 lines (script) | None | Data for N4 |
| 3 | N2: LU update quality tracking | ~15 lines | Low | Early degradation detection |
| 4 | N1-B: Shadow retry infrastructure | 4-6 hrs | Low | Root cause reduction |
| 5 | N4: Crisis recovery profiling | ~200 lines | Medium | Coherent framework |

N1-A and N3 are pure observation (no behavior change). N2 is additive.
N1-B depends on N1-A data. N4 depends on N3 data.

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

## Planned: Hard-Tail Closure — native NETLIB frontier vs GLPK

**Goal:** make native Ralph solve the ~18 numerically-hard NETLIB problems it currently
offloads to external GLPK (pilot family, maros-r7, d2q06c, greenbea/b, 25fv47, 80bau3b,
fit1p/2p, cycle, sierra, woodw). Measured against the native-only rows of
`ralph/benchmarks/netlib_perf_baseline.json`. Diagnosis complete (2026-09-10); no code
started. Frank write-up: `docs/roadmaps/ralph-vs-glop.md` (2026-09-10 addendum).

### Diagnosis (read-only sweep, native, hard-capped)

Three failure modes across the 18:

| Mode | Evidence | Sample |
|------|----------|--------|
| Dual breakdown | dual simplex → `ERROR` in 350–820ms, **presolve-independent** | 25fv47, pilot, 80bau3b, d2q06c, pilot87 |
| Primal cycling/stall | primal → `ITERATION_LIMIT` (honest non-convergence, no false optimum) | 25fv47, pilot |
| Primal correct-but-slow | converges 100–700× slower (80bau3b 172s vs GLPK 0.6s) | 80bau3b, maros-r7, d2q06c, pilot87 |

`AUTO` rescues none (picks a failing method). No native barrier
(`lp_backend.c`: "not implemented yet"). GLPK solves all 18 in <1s, primarily via a
robust dual simplex. So the gap is fundamentally **robustness**, not just speed.

### Work packages

- **P0: Instrument & root-cause (~2–4 days, low risk, read/measure only).** Pin the exact
  dual `ERROR` origin (LU breakdown vs dual ratio-test vs dual phase-1 basis); classify all
  18 by true mode; answer the load-bearing question — of the 15 "too slow," how many
  actually converge uncapped vs never?
- **P1: Dual simplex robustness — the spine (~3–6 weeks, high risk, hot core).** The dual
  errors fast and presolve-independent → a diagnosable core defect. Robust dual ratio test
  (bound-flipping/long-step), dual phase-1 / dual feasibility restoration, LU stability under
  the dual, kill the fast-breakdown. Highest impact: a working dual likely converts most of
  the 18 from FAIL to solved (as GLPK's does).
- **P2: Primal anti-cycling (~1–2 weeks, medium risk).** 25fv47/pilot stall despite the
  phase-1 recovery ladder. Expand-style tolerance relaxation and/or bound-flipping ratio test
  so degenerate problems terminate.
- **P3: Primal per-iteration performance (~2–3 weeks, medium risk).** Profiling hotspots:
  phase-1 pricing `O(candidates×artificials)` (pilot87), LU factorization (maros-r7),
  triangular solves (d2q06c). Steepest-edge/Devex as default hard-problem pricing, partial/
  sparse phase-1 pricing, warm-start/crash basis. Only helps where it already converges.
- **P4: Native barrier/interior-point (multi-month, very high effort — likely OUT of scope).**
  The only path to parity on the largest/densest (maros-r7, 151k nnz). Treat as a separate
  initiative, not part of "close the tail."
- **Cross-cutting:** determinism (`degen2` 492/492/537 run-to-run — same family as robustness)
  and presolve strength (GLPK reduces these more). Fold into P0/P1.

### Tiered goals (pick the bar)

- **Tier A — "no lies, no hangs" (~P0+P2, weeks):** native either solves or cleanly reports
  non-convergence without cycling/hanging; a fraction of the 18 solving. Removes the need to
  offload for *correctness*; still offload for *speed*.
- **Tier B — "native solves the tail" (~P0+P1+P2, ~2 months):** all 18 solved correctly
  natively within ~10–50× GLPK. The real "close the gap" target; external-GLPK fallback
  becomes optional.
- **Tier C — "parity" (+P3+P4, quarters):** within ~2–3× GLPK across the tail. Research-grade;
  barrier likely required.

### Guardrails

Every change verifies against: the NETLIB gate (0 status / 0 objective mismatch), the
presolve/infinite-bound/equality fuzzers + GLPK oracle, and the native-only rows of
`netlib_perf_baseline` as the before/after. Verify-first, same discipline as #86/#94/#96 —
mandatory here since it is the hot core.

### Honest sizing

Not a quick win. A robust dual simplex is exactly what took GLPK/HiGHS/Clp years. Tier B is a
~2-month focused effort with real regression risk in the most sensitive code; Tier C is
multi-quarter. Cheapest genuinely-valuable milestone is Tier A (stop the cycling/hangs).

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
ralph/api/src/main.c           # Keel HTTP wrapper (port 8084)
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

## Keel Migration — Phase 2 of 6 (✅ Complete)

**Completed for Ralph.** `ralph/api` runs on Keel v3.

Rationale and the shared context are in `docs/roadmaps/surge.md` (Phase 1): the
previous server was `GPL-2.0-only or commercial`, which is incompatible with
OTTO's AGPL-3.0 and unsublicensable for the commercial tier. Keel is MIT.

### Shape of the port

Ralph's transport layer is a thin dispatcher — it marshals a `RalphAPIRequest`
and hands everything to `ralph_api_handle()`, which owns all routing. That is
preserved exactly:

- Three routes (`/api/v1/health`, `/api/v1/formats`, `/api/v1/solve`) all
  forward to a single `dispatch()`.
- `mw_fallback` catches anything the route table misses and forwards it to the
  *same* `dispatch()`, so unknown paths and wrong methods get Ralph's own
  `{"error":"Endpoint not found"}` rather than a transport-invented body. This
  is required because Keel route patterns have no wildcard (`*` is only special
  in middleware patterns) and Keel's built-in 404 is `text/plain` with no CORS.
- CORS headers are the same four the previous server emitted on every response.

Ralph needs **no** `sh_httpserver.c` helpers — it uses neither `sh_cors`,
`sh_metrics`, `sh_ratelimit` nor `sh_trace`. Its handlers are fully
synchronous, so no `KlAsyncOp`/`KlThreadPool` wiring either. This made it the
cheapest second module.

### Two pre-existing issues found while verifying — now fixed

**1. The API test could not fail.** The `test:` target echoed `PASS`/`FAIL` and
ended with `kill`, never exiting non-zero, so `make -C ralph/api test` was green
regardless of what the assertions found. Replaced with
`ralph/api/test_api.sh`, which starts the server itself and exits non-zero on
failure. CI runs the script directly.

**2. The status-case mismatch was in the test, not the API.**

An earlier note here claimed Ralph should be uppercased to match Surge. That
was wrong. Ralph's HTTP and WASM APIs are **documented as lowercase** —
`ralph/api/CLAUDE.md` and `ralph/wasm/CLAUDE.md` both show
`"status": "optimal"` — and `status_to_json()` carries an explicit
`lowercase for JSON` comment. The old Makefile target grepped for
`"status":"OPTIMAL"`, which never matched, which is why it printed
`FAIL: Solve endpoint` while returning the correct answer.

The new suite asserts the documented lowercase form. **No API behaviour
changed.**

Note the C library is a separate surface and does use uppercase
(`ralph_netflow_status_string()` returns `"OPTIMAL"`, and `ralph/tests/` assert
that). Library-uppercase / JSON-lowercase is a deliberate split, not a bug.

That said, JSON status case is inconsistent *across modules*: Ralph is
lowercase while FuelWise and Surge are uppercase. Worth settling one way, but
it is an API-contract decision touching documented behaviour, not a cleanup.

### Status: Complete

All six servers are on Keel v3; the previous GPL HTTP server has been removed.
See `docs/roadmaps/infrastructure.md` for the cross-cutting completion record
(including the `sh_keelserver`/`sh_keelasync` → `sh_httpserver`/`sh_httpasync`
rename).
