# Ralph Solver Roadmap

Development roadmap for Ralph LP/MIP solver covering algorithms, performance, and planned features.

## Stable Baseline

**Current** (2026-02-22, `beac5bb`) — Phase 2 LP/MIP boundary extraction baseline:
introduced a dedicated MIP/LP adapter surface (`mip_lp_adapter`) and moved MIP node/probing LP
state transitions behind adapter operations (`apply bounds`, `recompute`, `dual reopt`,
`warm restore`, `cold recover`) to reduce direct tableau lifecycle mutation in `mip.c` and
`branch_bound.c`. Added branching tightening safety guards to prevent duplicate non-tightening
child chains after probing state drift, plus a targeted regression test
(`test_branch_tightening_guard`). Latest gates:
`make -C ralph test-netlib-gate-small` PASS (26 files, dense fallback files: 0, no unexpected
regressions, artifacts: `/tmp/netlib-regression-gate-20260222-174924`) and
`make -C ralph test-netlib-gate` PASS (84 files, 27 known timeouts, status/objective/invalid
mismatches 0, dense fallback files: 0, no unexpected regressions, artifacts:
`/tmp/netlib-regression-gate-20260222-175112`).

Previous: (2026-02-22, `d093284`) — logging + telemetry architecture baseline:
introduced a shared LP logging shim (`lp_log`) backed by `sh_log`, migrated verbose solver output
paths in `simplex.c`, `dual_simplex.c`, and `mip.c` off direct `printf`/`fprintf`, and added
timed telemetry wrappers in `lp_telemetry` so solver/LU timing callsites use a single coherent
timer/recording interface. Also added LP+MIP integration coverage for runtime telemetry parameter
propagation (`telemetry=0/1`) including LU/node-LP propagation checks. Latest gates:
`make -C ralph test-simplex-policy` PASS (20/20), `make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-lp-telemetry` PASS (58/58), and full `make -C ralph test` PASS
(`test_ralph` 398/398, presolve/netlib parser gates PASS).

Previous: (2026-02-21, `2ef9ac4`) — H5 adaptive Devex-partial pricing baseline:
added adaptive Devex-partial pricing for large, degenerate Phase 2 workloads with periodic full
Devex rescans to preserve robustness while reducing pricing cost on heavy NETLIB outliers. Latest gates:
`make -C ralph test-simplex-policy` PASS (16/16), `make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0), and full
`make -C ralph test-netlib-gate` PASS (84 files; 27 known timeouts; status mismatch 0; objective
mismatch 0; invalid solution 0; dense fallback files 0; no unexpected regressions vs baseline).
GLPK comparison snapshot from `/tmp/netlib-regression-gate-20260221-094637`: both-optimal set 57 files,
geometric mean time ratio (`Ralph/GLPK`) 1.38x, geometric mean iteration ratio 0.98x, geometric mean
per-iteration ratio 1.41x.

Previous: (2026-02-20, `738a2fe`) — H4 simplex pivot/ratio kernel baseline:
optimized hot simplex kernels in `simplex_pivot` and `ratio_test_harris` (pointer-hoisting and fused
basic-variable update + direction-norm accumulation) while preserving numerical behavior. Latest gates:
`make -C ralph test-simplex-policy` PASS (16/16), `make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0), and full
`make -C ralph test-netlib-gate` PASS (84 files; 27 known timeouts; status mismatch 0; objective
mismatch 0; invalid solution 0; dense fallback files 0; no unexpected regressions vs baseline).

Previous: (2026-02-20, `25e0b93`) — Phase-1 direction-stabilize cooldown baseline:
added a bounded cooldown gate for `RALPH_REFACTOR_REASON_DIRECTION_STABILIZE` in primal Phase 1 so
large-direction handling does not refactor on nearly every iteration, while preserving hard LU-safety
override conditions (`lu_needs_refactorization` and extreme-direction forcing). Latest gates:
`make -C ralph test-simplex-policy` PASS (16/16), `make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0), and full
`make -C ralph test-netlib-gate` PASS (84 files; 27 known timeouts; status mismatch 0; objective
mismatch 0; invalid solution 0; dense fallback files 0; no unexpected regressions vs baseline).

Previous: (2026-02-20, `ece147e`) — NETLIB MPS parser + objective-offset correctness baseline:
fixed fixed-format MPS edge cases (embedded-space names, optional BOUNDS set name), preserved imported
variable names, and added RHS objective-row constant handling (`obj_offset`) to parser + simplex/dual
objective reporting. Latest gates: `make -C ralph test` PASS (includes new
`test_mps_parser_netlib`), `make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0),
and full NETLIB gate on 84 files now has status mismatch 0, objective mismatch 0, invalid solution 0,
dense fallback files 0; `forplan.mps` is now a known timeout under the 20s hard cap (27 known timeouts).

Previous: (2026-02-20, `eabbaa8`) — H3 adaptive Markowitz retry baseline + full NETLIB/GLPK refresh:
adapted Markowitz pool multiplier on retries to cut retry churn while preserving sparse-LU-first behavior.
Latest full gate: `make -C ralph test-netlib-gate` PASS (84 files, 26 known timeouts, dense fallback
files: 0, no unexpected regressions vs baseline). Known baseline mismatches remain:
status mismatch 2 (`forplan.mps`, `sierra.mps`), objective mismatch 3 (`e226.mps`, `forplan.mps`,
`sierra.mps`), invalid solution 3 (same set). GLPK comparison snapshot from
`/tmp/netlib-regression-gate-20260220-143400`: both-optimal set 56 files, geometric mean time ratio
(`Ralph/GLPK`) 1.44x, geometric mean iteration ratio 1.03x, geometric mean per-iteration ratio 1.40x.

Previous: (2026-02-20, `c844cbe`) — H1c + H2 Markowitz refactor baseline:
added adaptive reach-mask sparse transpose solves (H1c), then reduced Markowitz numeric refactor
work with pivot-row-scoped cleanup and cached row-to-column position hints. Latest gates:
`make -C ralph test-simplex-policy` PASS (16/16), `make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0, no unexpected
regressions vs baseline). H2 focus snapshot (vs `/tmp/h2base_*.json`): `refactor.all_ms`
improved on `25fv47` (-40.22%), `fit1p` (-15.00%), `80bau3b` (-74.52%), `nesm` (-72.58%),
`czprob` (-75.13%), with status/objective validation preserved on all five.

Previous: (2026-02-20, `eefe861`) — Phase 5 FT-chain baseline + GLPK comparison refresh:
batched FT spike micro-kernels for long update chains are now baseline, with roadmap/docs aligned.
Latest gates: `make -C ralph test` PASS, `make -C ralph test-simplex-policy` PASS (16/16),
`make -C ralph test-lu-markowitz` PASS (59/59), `make -C ralph test-netlib-gate-small` PASS
(26/26, dense fallback files: 0), and full NETLIB gate baseline parity (84 files; no unexpected
regressions; known pre-existing `bnl1` command failure remains). GLPK comparison snapshot from
`/tmp/netlib-regression-gate-20260220-121147`: comparable both-optimal set 55 files, geometric
mean time ratio (`Ralph/GLPK`) 1.48x, geometric mean iteration ratio ~1.00x, geometric mean
per-iteration ratio 1.48x.

Previous: (2026-02-20, `f068c66`) — LP periodic scheduler feedback baseline:
unified pressure scheduler now includes bounded per-phase adaptive feedback from observed
periodic refactor outcomes while preserving hard LU safety triggers and no-regression canaries.
Latest gates: `make -C ralph test-simplex-policy` PASS (16/16),
`make -C ralph test-lu-markowitz` PASS (59/59),
`make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0),
and canary gate `fit1p|nesm|bandm|scagr25` PASS (dense fallback files: 0).

Previous: (2026-02-20, `59958a4`) — LP sparse refactor baseline hardening:
persisted symbolic identity workspaces (removes per-refactor malloc churn), enabled sparse
symbolic `k=m` fast-path to avoid avoidable dense fallback, and hardened identity-separation
numeric flow with retry/stage telemetry coverage updates.
Latest gates: `make -C ralph test` PASS, `make -C ralph test-netlib-gate-small` PASS (26/26,
dense fallback files: 0, no unexpected regressions vs baseline).

Previous: (2026-02-20, `5ad46a9`) — LP refactor telemetry baseline:
stage-level LU timing/counters (symbolic cache, sparse numeric split, dense factorization timing),
sparse fallback reason telemetry (`small_matrix` / `symbolic` / `numeric`), and removed symbolic
`num_identity >= m/4` cutoff to avoid unnecessary dense fallback on low-identity bases.
Latest gates: `make -C ralph test` PASS, `make -C ralph test-netlib-gate-small` PASS (26/26, dense fallback files: 0).

Previous: (2026-02-20, `173d430`) — Unified pressure-based periodic LU scheduler (single path),
Markowitz sparse LU default, and NETLIB no-regression gate with required-pass canaries:
`bandm`, `scagr25`, `fit1p`, `nesm`.
Latest canary gate run: 4/4 PASS, no dense fallback, no timeout regressions (including `fit1p`/`nesm`).

Previous: (2026-02-18, `165fe05`) — LP performance: CSR row-scatter for sparse RC updates,
supernodal LU auto-enabled for m>300, conservative refactorization for m≥500, Markowitz LU.
NETLIB fast tier: 22/22 PASS, 1 ERROR (share1b), 2 SKIP (bore3d, capri). All 378 tests pass.

Previous: (2026-02-17) — Anti-degeneracy: primal Phase 1/2 stall detection with progressive
re-perturbation, expanded dual perturbation budget (5→20), proactive perturbation on dual fallback.
NETLIB fast gate: 23/23 PASS, 1 ERROR (share1b), 1 SKIP (bore3d). recipe 89s→2.8ms (was 11546x GLPK,
now 0.5x). scagr25 now solves. beaconfd excluded (tier 5). All 378+ unit tests pass.

Previous: (2026-02-17) — LP gap closure: sparse BTRAN, dual refinement, runtime tolerances.
Previous: `85a5295` — Week 2 Devex fix + Phase 1/2 pricing robustness.
Previous: `a67f09f` — Week 1 LP perf: B1-B6 low-hanging fruit, net -138 LoC.
Previous: `5ccab2e` — NETLIB suite extended to 84 problems, full test infrastructure.
Previous: `f80487e` — Phase E perf fix: 500-iter budget + primal cold-start.
Previous: `140a1f2` — Phase E: replace dual_reopt with clean dual simplex in MIP.
Previous: `7d78375` — Phase D: dual simplex default, 90% SotA.
Previous: `9315fd3` — Strong branching UAF fix + NaN safety + RC fixing + RINS.
Previous: `64f6cdc` — Cut generation normalization fix + pseudocost branching + probing.
Previous: `af158fa` — P5/P6 re-landed with infeasibility guards (208 tests, 60/60 MILP).
Previous: `fc454a7` — c-MIR sign fixes + infeasibility guard (199 tests, 100/100 MILP).
Previous: `b1d0e8c` — Objective cutoff + lightweight presolve with priority remapping.
Previous: `4387869` — HYBRID + PATH B LU reuse (9x milp15, 1.9x milp30).

## Status Summary (Feb 2026)

| Area | Status | Notes |
|------|--------|-------|
| **Revised Simplex** | ✅ Complete | Primal + dual simplex, two-phase (no Big-M), 6 pricing strategies (Dantzig/SE/Devex/Partial/Heap/SE+Devex) |
| **LU Factorization** | ✅ Complete | Sparse Markowitz + FT updates + symbolic/numeric separation (T1.4) + sparse BTRAN + supernodal (T2.1) |
| **Branch & Bound MIP** | ✅ Complete | Reliability branching, GMI+cMIR cuts, RC fixing, RINS, pseudocosts |
| **Dual Simplex** | ✅ Complete | Bound flipping (P5), dual steepest edge (P6), two-sided perturbation, from-scratch + warm-start |
| **LAP Solver** | ✅ Complete | JVC algorithm, 358 tests |
| **Network Flow** | ✅ Complete | Network simplex, 153 tests |
| **Problem Detection** | ✅ Complete | Auto-detect LAP/network structure, 194 tests |
| **Presolve** | ✅ Phase 3 | 14 techniques incl. redundant rows, SCP-specific, 105 tests |
| **NETLIB Suite** | 92% T0-1 | 23/23 fast pass (anti-cycling fixed recipe/scagr25; beaconfd excluded; bore3d timeout; share1b iter-limit), 84 problems |
| **MIP Infrastructure** | ✅ Complete | Branching, cuts, callbacks, warm start (§6) |
| **Benders Decomposition** | ✅ Complete | Generic solver, ~1430 LoC, 8 tests (§7) |

---

## LP-First Robustness and Separation Plan (2026-02-22)

Goal: harden LP correctness/performance first, then enforce a clean LP/MIP architectural boundary so MIP cannot destabilize LP internals.

Design principles:
- LP robustness is the primary gate; MIP changes do not bypass LP NETLIB regression gates.
- LP and MIP stay orthogonal at API and implementation boundaries.
- MIP must consume LP through stable warm-start/relaxation interfaces, not direct tableau mutation.

### Phase 1 (Start Here): LP Regression Hardening Gate

Scope:
- Re-run deterministic LP NETLIB gates as first-class CI barriers before any MIP refactor.
- Baseline lock requirements: no new status/objective/invalid-solution regressions, no new timeout regressions, zero dense sparse-LU fallback regressions.

Execution:
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

Exit criteria:
- Gate scripts pass against `benchmarks/netlib_regression_baseline.json`.
- Artifacts captured and referenced in this roadmap.

Progress (2026-02-22):
- `make -C ralph test-netlib-gate-small` PASS
  - Artifacts: `/tmp/netlib-regression-gate-20260222-171201`
  - Summary: 26 files, timeout 4, command failures 0, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.
- `make -C ralph test-netlib-gate` PASS
  - Artifacts: `/tmp/netlib-regression-gate-20260222-171349`
  - Summary: 84 files, timeout 27, command failures 0, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.

### Phase 2: LP/MIP Implementation Boundary Extraction

Scope:
- Introduce an internal LP-relaxation adapter for MIP node operations (solve/reopt/probe/warm-start).
- Remove direct `tableau_*` mutation from MIP/branching codepaths where practical; centralize LP state transitions in one module.

Exit criteria:
- MIP compiles/runs through adapter paths.
- No LP regression on NETLIB gates.

Progress (2026-02-22):
- Implemented internal MIP/LP adapter (`ralph/src/mip_lp_adapter.h`) and rewired MIP node/refactor/probing paths in:
  - `ralph/src/mip.c`
  - `ralph/src/branch_bound.c`
- Removed direct tableau lifecycle mutation from these MIP paths in favor of adapter calls (`apply bounds`, `recompute`, `dual reopt`, `warm restore`, `cold recover`).
- Added branching safety guards to prevent duplicate non-tightening child chains:
  - Post-probing fractional re-check fallback before branching (`mip.c`)
  - Child-creation tightening checks (`branch_bound.c`)
- Added targeted regression unit test:
  - `test_branch_tightening_guard` in `ralph/tests/test_main.c`
- Regression gates after Phase 2 changes:
  - `make -C ralph test-netlib-gate-small` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-174924`
    - Summary: 26 files, timeout 4, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.
  - `make -C ralph test-netlib-gate` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-175112`
    - Summary: 84 files, timeout 27, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.

### Phase 3: API-Level Separation (Non-Breaking)

Scope:
- Keep `ralph_optimize()` compatibility while adding explicit LP and MIP optimize entry points.
- Separate LP-only and MIP-only parameter surfaces while preserving existing string-parameter compatibility.

Exit criteria:
- Existing callers remain source-compatible.
- New tests verify LP-only APIs do not instantiate MIP paths and vice versa.

Progress (2026-02-22):
- P3.1/P3.2 implemented:
  - Added explicit optimize entry points in public API:
    - `ralph_optimize_lp()`
    - `ralph_optimize_mip()`
  - Kept `ralph_optimize()` as compatibility dispatcher.
  - Added strict mode behavior:
    - `ralph_optimize_lp()` rejects integer models (`RALPH_STATUS_ERROR`).
    - `ralph_optimize_mip()` rejects pure LP models (`RALPH_STATUS_ERROR`).
  - Added tests in `ralph/tests/test_main.c`:
    - `test_phase3_optimize_entrypoints` verifies LP path does not create MIP solver
      and explicit MIP path creates MIP solver only for integer models.
- P3.3 implemented:
  - Added strict LP/MIP parameter APIs (non-breaking additions):
    - LP strict: `ralph_set/get_lp_{int,dbl}_param()`
    - MIP strict: `ralph_set/get_mip_{int,dbl}_param()`
  - Added routing/rejection tests:
    - `test_phase3_param_partition`
- P3.4 implemented:
  - Added backward-compatibility regression test:
    - `test_phase3_optimize_backward_compatibility`
  - Confirms legacy `ralph_optimize()` + string params match explicit LP/MIP entry-point
    status/objective on representative LP and MIP fixtures.
- Regression gates after Phase 3 changes:
  - `make -C ralph test-netlib-gate-small` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-183622`
    - Summary: 26 files, timeout 4, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.
  - `make -C ralph test-netlib-gate` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-183809`
    - Summary: 84 files, timeout 27, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.

Plan (start, 2026-02-22):
1. API entry-point split (P3.1)
- Add explicit optimize entry points in public API:
  - `ralph_optimize_lp()`
  - `ralph_optimize_mip()`
- Keep `ralph_optimize()` as compatibility dispatcher with current behavior.
- Gate: compile + `make -C ralph test`.

2. Solver creation path split (P3.2)
- Ensure LP optimize path cannot instantiate or mutate MIP solver state.
- Ensure MIP optimize path owns MIP solver lifecycle explicitly.
- Add focused tests:
  - LP path leaves `ralph_get_mip_solver(model)` null/unchanged.
  - MIP path builds MIP solver only for integer models.
- Gate: `make -C ralph test`, `make -C ralph test-netlib-gate-small`.

3. Parameter surface partition (P3.3)
- Add explicit LP param setters/getters and MIP param setters/getters (non-breaking additions).
- Keep string param APIs as compatibility wrappers with clear routing.
- Add validation tests for parameter routing and rejection of wrong-domain params in strict APIs.
- Gate: `make -C ralph test`.

4. Backward-compatibility verification (P3.4)
- Keep all existing callers/source signatures valid.
- Add regression test that old `ralph_optimize()` + string params produce unchanged statuses/objectives
  on representative LP/MIP fixtures.
- Gate: `make -C ralph test-netlib-gate`, then refresh baseline entry.

### Phase 4: MIP Best-Practice Alignment with Current LP Warm-Start Infrastructure

Scope:
- Align node warm basis/MIP start behavior with shared LP warm-start lifecycle.
- Ensure strong-branch probing and cut loops are side-effect safe with explicit LP state recovery contracts.

Exit criteria:
- New targeted tests for probe safety, warm-start acceptance/rejection, and LP-state recovery.
- No regressions on NETLIB gates and focused FuelWise MILP seeds.

Progress (2026-02-22):
- Added explicit LP recovery contract for root-cut fallback:
  - `mip_recover_root_relaxation()` in `ralph/src/mip.c`
  - contract: rebuild working LP from original model, recreate LP solver, re-solve root relaxation,
    and return success only with usable OPTIMAL LP state.
- Cut-loop fallback now uses the explicit recovery contract (instead of ad-hoc inline rebuild logic).
- Strong-branch probing now enforces an explicit post-probe LP-state contract:
  - restores original structural bounds + warm basis on failure paths,
  - falls back to adapter-based LP recovery (`mip_lp_recover_state`) if restore fails,
  - tracks probe/recovery telemetry counters.
- Added Phase 4 focused regression test:
  - `ralph/tests/test_mip_lp_recovery.c`
  - coverage:
    - strong-branch probe safety (basis/bounds restored, LP state usable after probing),
    - root LP recovery contract behavior after working-model corruption.
- Added telemetry counters in `MIPSolver` for probe/cut recovery paths:
  - `strong_branch_probes`, `strong_branch_failures`, `strong_branch_recoveries`
  - `cut_recovery_attempts`, `cut_recovery_success`, `cut_recovery_failures`
- Make/test integration:
  - new target `test-mip-lp-recovery`
  - included in `make -C ralph test` dependency list.

Validation (2026-02-22):
- `make -C ralph test-mip-lp-recovery` PASS (32/32).
- Warm-start acceptance/rejection coverage still passes:
  - `make -C ralph test-mip-warmstart` PASS (22/22)
  - `make -C ralph test-mip-start-repair` PASS (24/24)
  - `make -C ralph test-mip-start-sparse` PASS (12/12)
- NETLIB regression gates:
  - `make -C ralph test-netlib-gate-small` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-191305`
    - Summary: 26 files, timeout 4, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.
  - `make -C ralph test-netlib-gate` PASS
    - Artifacts: `/tmp/netlib-regression-gate-20260222-191452`
    - Summary: 84 files, timeout 27, status/objective/invalid mismatches 0, dense fallback files 0, unexpected regressions 0.

### Regression rule for every phase

- `make -C ralph test`
- `make -C ralph test-netlib-gate-small`
- At phase completion: `make -C ralph test-netlib-gate`

## API Production Hardening Track (Feb 2026)

Goal: close API-contract and operability gaps against production-grade LP/MIP APIs
(CPLEX/HiGHS/GLPK-class expectations) without destabilizing current solver baselines.

### Phase P0 (highest impact / lowest effort)

Progress (2026-02-22): P0.1-P0.3 implemented and regression-gated.

1. Contract hardening ✅
- Align JSON API status contract with tests (status string casing/shape).
- Close declared-but-missing symbols (`ralph_write_mps`).
- Run gates: `make -C ralph test-api`.

2. Regression gate coverage ✅
- Include API tests in default Ralph test gate.
- Run gates: `make -C ralph test`.

3. I/O contract coverage ✅
- Add explicit write/read MPS round-trip tests in LP format suite.
- Run gates: `make -C ralph test-lp`, then `make -C ralph test`.

### Phase P1

Progress (2026-02-22): P1.1 (coefficient-edit APIs + explicit invalidation semantics) and P1.2 (staged basis load lifecycle) implemented and regression-gated.

1. Incremental reoptimization API ✅ P1.1 done
- Add coefficient-edit APIs (`set_aij`, bulk sparse updates). ✅
- Add explicit row/column removal and mutation-state invalidation semantics. ✅
- Document modify->resolve lifecycle guarantees. ✅

2. Warm-start lifecycle completion ✅ P1.2 done
- Support staged basis loading before first optimize call. ✅
- Add behavior tests for pre-solve and post-solve basis load paths. ✅

### Phase P2

1. Callback/event model expansion
- Add progress callback (iterations/nodes/time), incumbent callback, and cancellation callback.
- Add log callback routing (shared logging backend) for embedding contexts.

2. MIP advanced starts
- Add MIP start API (full and partial hints) with acceptance diagnostics.

### Phase P3

1. Diagnostics parity
- Add IIS/conflict API for infeasible models.
- Add unbounded primal ray API.
- Expose solution-quality/KKT residual metrics.

### Phase P4

1. Parameter system modernization
- Add typed parameter enum API alongside current string API.
- Add parameter metadata/introspection (name/type/default/range/description).
- Add deterministic/reproducibility controls in public API contract.

Execution rule: each sub-phase lands behind deterministic regression gates before moving forward.

### Consolidated LP API Parity Plan (execute in order)

Goal: close LP-only API gaps versus production-grade solver APIs while preserving current
LP/MIP correctness and performance baselines. This is the execution order to address items
one by one.

0. Baseline lock + gate discipline
- Keep the current NETLIB/API baseline as the reference.
- Required gate after every item:
  - `make -C ralph test`
  - `make -C ralph test-netlib-gate-small`
- Additional gate every 2 items (or when API contracts change):
  - `make -C ralph test-netlib-gate`

1. Presolve report API (highest impact, low risk) ✅ (2026-02-22)
- Add public `RalphPresolveReport` to `ralph.h`.
- Add `ralph_get_last_presolve_report()` for last solve.
- Expose at minimum: `used`, `mask`, `rounds`, `vars_removed`, `cons_removed`,
  `bounds_tightened`, `matrix_rank`, `redundant_rows_found`, `presolve_time_ms`.
- Add unit tests for LP and MIP solve paths with presolve on/off.

2. Public LP/LU telemetry snapshot API ✅ (2026-02-22)
- Add public API-safe snapshot structs and getters in `ralph.h`.
- Wrap existing internal telemetry snapshots so callers do not include `lp.h`.
- Add tests for `telemetry=1` and `telemetry=0` behavior.

3. Public solution-quality/KKT API ✅ (2026-02-22)
- Add `ralph_get_solution_quality()` to expose verification metrics:
  `primal_infeas`, `bound_infeas`, `dual_infeas`, `comp_slack`, `obj_error`,
  `cond_estimate`.
- Define clear availability contract by status and test it.

4. LP progress + cancellation callbacks
- Add LP progress callback (iteration/time/objective/quality summary).
- Add cancellation callback/poll hook for long LP solves.
- Ensure LP callback path is orthogonal to MIP callback path.

5. LP model query/edit ergonomics completion
- Add missing constraint/coef query APIs:
  `get_constraint_rhs`, `get_constraint_sense`, `get_constraint_coef` (or sparse row read).
- Add explicit row/column deletion APIs with documented invalidation semantics.
- Add batch edit APIs with all-or-nothing validation.

6. LP diagnostics parity (infeasible/unbounded)
- Add public unbounded primal ray API for LP unbounded status.
- Add LP IIS/conflict API (initial minimal irreducible row set is acceptable as first version).
- Add deterministic tests for infeasible/unbounded fixtures.

7. Basis-status API (beyond opaque basis blob)
- Add explicit basis status getters/setters (row/column/basic/nonbasic states).
- Keep `ralph_save_basis`/`ralph_load_basis` for compatibility.
- Add round-trip and dimension-mismatch tests.

8. Typed parameter API + metadata
- Add enum-based typed parameter APIs alongside existing string APIs.
- Add parameter metadata/introspection API:
  `name`, `scope` (LP/MIP/shared), `type`, `default`, `min/max`.
- Keep string APIs as compatibility wrappers; add parity tests.

9. Determinism/reproducibility contract (LP)
- Add explicit reproducibility controls in public API (seed/deterministic mode/thread policy).
- Document guarantees and known non-deterministic cases.
- Add deterministic regression tests.

10. Advanced parity backlog (separate track)
- Sensitivity/ranging API (objective, RHS, bounds ranges).
- Stronger IIS/conflict refinement.
- Optional barrier/crossover API surface (if adopted in solver core).

Exit criteria for this consolidated plan:
- No regressions against NETLIB gates or existing API suites.
- New APIs are LP/MIP-orthogonal by design, with focused unit tests per feature.
- Existing callers using current API remain source-compatible.

---

## Chapter 1: LP Solver Performance

### 1.1 Current Benchmarks

**1000x500 LP (15% dense)**
| Solver | Time | Per-Iteration | Gap |
|--------|------|---------------|-----|
| Ralph  | 0.41s | ~0.17ms | - |
| GLPK   | 0.12s | 0.044ms | ~3.4x |

**Per-Iteration Breakdown (post sparse BTRAN):**
- 42% LU factorization (refactorization) — supernodal (T2.1) implemented
- 15% BTRAN (sparse DFS-based, was 27%)
- 26% FTRAN (hyper-sparse DFS)
- 17% Other (pricing, ratio test, refinement)

### 1.2 NETLIB Benchmark Results (Feb 2026, post Markowitz + LP-perf)

22/22 fast-tier PASS. 2 SKIP (bore3d, capri timeout). 1 ERROR (share1b iter-limit).

| Problem | Status | Ralph ms | GLPK ms | Ratio | Notes |
|---------|--------|----------|---------|-------|-------|
| afiro | ✅ PASS | 0.2 | ~0* | ~1x | Trivial |
| blend | ✅ PASS | 3.2 | ~0* | ~1x | Fixed by method=2 |
| kb2 | ✅ PASS | 0.8 | ~0* | ~1x | Small dense |
| sc50a/b | ✅ PASS | 0.4 | ~0* | ~1x | Small |
| sc105 | ✅ PASS | 2.1 | ~0* | ~1x | Small |
| recipe | ✅ PASS | 1.9 | ~0* | ~1x | Fixed from 89s cycling |
| adlittle | ✅ PASS | 1.5 | ~0* | ~1x | Small |
| share2b | ✅ PASS | 2.9 | ~0* | ~1x | Small |
| stocfor1 | ✅ PASS | 3.5 | ~0* | ~1x | Stochastic |
| scagr7 | ✅ PASS | 3.9 | ~0* | ~1x | Small |
| lotfi | ✅ PASS | 15.9 | 7.9 | 2.0x | Was 237ms pre-Markowitz |
| grow7 | ✅ PASS | 18.5 | ~0* | ~1x | Medium |
| israel | ✅ PASS | 19.5 | 9.0 | 2.2x | Medium |
| sc205 | ✅ PASS | 24.9 | ~0* | ~1x | Medium |
| brandy | ✅ PASS | 34 | 10.9 | 3.1x | Medium degenerate |
| e226 | ✅ PASS | 52 | 11.6 | 4.5x | Was 101ms |
| scorpion | ✅ PASS | 122 | 9.1 | 13x | Large degenerate |
| agg | ✅ PASS | 126 | 8.6 | 15x | Large |
| agg2 | ✅ PASS | 276 | — | — | Large |
| bandm | ✅ PASS | 345 | 12.9 | 27x | Was 7200ms pre-Markowitz |
| agg3 | ✅ PASS | 356 | — | — | Large |
| scagr25 | ✅ PASS | 460 | 7.4 | 62x | Was timeout |
| share1b | ❌ ERROR | — | — | — | Iter-limit (status=4) |
| bore3d | ⏭ SKIP | — | — | — | Timeout |
| capri | ⏭ SKIP | — | — | — | Timeout |

*GLPK sub-ms problems show process startup overhead (~7ms wall clock); actual solve is sub-ms.

**Gap summary:** Competitive on small (≤sc105). 2-3x on medium (lotfi, israel, brandy). 5-60x on
large (e226-scagr25). bandm dramatically improved from 578x to 27x thanks to Markowitz LU.
Remaining levers: per-iteration cost (FTRAN/BTRAN spike application), cost perturbation (D5).

### 1.3 Performance TODO

See **§1.12** for the full state-of-the-art gap analysis with prioritized implementation order.

| Priority | Task | Expected Impact | §1.12 ID |
|----------|------|-----------------|----------|
| ✅ **Done** | Multi-round equilibrium scaling | Better numerics | T1.2 |
| ✅ **Done** | Triangular crash basis | 2-5x cold starts | T1.1 |
| ✅ **Done** | Symbolic/numeric LU separation (§1.11) | 1.5-2x refactorization | T1.4 |
| ✅ **Done** | Supernodal LU factorization (§1.11) | 3-5x factorization | T2.1 |
| ✅ **Done** | Heap-based pricing (DynamicMaximum) | 2-5x pricing on large problems | T2.2 |
| ✅ **Done** | Post-solve verification | Correctness, catch silent failures | T2.3 |
| ✅ **Done** | Objective limits in simplex_solve | 30-50% fewer pruned-node iters | T3.1 |
| ✅ **Done** | Dual simplex as default LP | ~2x initial solves (arch change) | T1.3 |
| ✅ **Done** | Sparse BTRAN (DFS-based) | 2-5x per-iteration on m>100 | W1 |
| ✅ **Done** | Dual iterative refinement | Numerical stability | W4 |
| ✅ **Done** | Two-sided dual perturbation | Better anti-cycling | W5 |
| ✅ **Done** | Runtime-configurable tolerances | User tuning | W2 |
| ✅ **Done** | SE+Devex-init hybrid pricing | 10-30% fewer iters on degenerate | W3 |
| ✅ **Done** | Anti-degeneracy (D1/D2/D4) | recipe 11546x→0.5x, scagr25 fixed | — |
| ✅ **Done** | Row-form (CSR) RC update | Density-gated (<2%); czprob -16% | B7 |
| ✅ **Done** | Conservative refactorization | m≥500: 100 updates, spike-work trigger | §8.6 |

### 1.4 Numerical Stability TODO

| Task | Status | Notes |
|------|--------|-------|
| Two-Phase Simplex | ✅ Complete | For >80% equality problems |
| Geometric Mean Scaling (1 round) | ✅ Complete | `apply_scaling()` in simplex.c |
| Multi-Round Equilibrium Scaling | ✅ Complete | 5 geometric + 20 equilibrium (§1.12 T1.2) |
| Iterative Refinement (primal) | ✅ Complete | Residual correction |
| Iterative Refinement (dual) | ✅ Complete | W4: rc[j] = c[j] - A^T y recomputation in `verify_solution()` |
| Threshold Pivoting | ✅ Complete | In factorization + updates |
| Harris Ratio Test | ✅ Complete | In both primal and dual |
| Bound Perturbation | ✅ Complete | Primal + dual, reactive + proactive, two-sided (W5) |
| Runtime Tolerances | ✅ Complete | W2: `feas_tol`, `opt_tol`, `pivot_tol` via `ralph_set_dbl_param()` |
| Cost Perturbation | ⏳ TODO | Alternative anti-degeneracy (§1.12 T3.3) |
| Post-Solve Verification | ✅ Complete | Primal/dual feasibility + complementary slackness (§1.12 T2.3) |
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

**Current results (post Phase E + perf fix, seed 42, 5 runs, `f80487e`):**

| Scenario | Ralph avg | GLPK avg | Speed | Obj match | Gap avg |
|----------|-----------|----------|-------|-----------|---------|
| milp15 | 20 ms | **7.8 ms** | 0.4x | 3/5 | 124.5% |
| milp30 | 176 ms | **9.0 ms** | 0.05x | 4/5 | 0.21% |
| milp50 | 1,581 ms | **13 ms** | 0.01x | 3/5 | 36.5% |
| milp75 | 5,527 ms | **59 ms** | 0.01x | 4/5 | 0.20% |
| milp100 | 39,210 ms | **18 ms** | 0.0x | 2/5 | 7.2% |
| milp200 | 223,675 ms | **91 ms** | 0.0x | 2/5 | 5.9% |

Single-seed (42), 5 runs each. Gap = `(ralph - glpk) / |glpk| × 100%`. 100% solve rate.

**Key observations (post Phase E):**
- **Phase E regression fixed** but overall speed is WORSE than pre-Phase-E due to `dual_simplex_solve_v2`
  overhead vs old `dual_reopt` (exact DSE init, bound perturbation, unshift cleanup per node).
- **milp15/milp50** have catastrophic objective gaps (124.5% / 36.5%) — branching quality issue.
- **milp30/milp75** have good objective quality (<0.25% avg).
- **Speed gap is enormous at milp50+**: GLPK is 100-2000x faster. Root cause: Ralph's per-node
  LP solve cost is O(m²) with v2 (perturbation + DSE + unshift) vs GLPK's optimized warm-start.
- The old dual_reopt was faster for MIP (approximate DSE, no perturbation, no unshift) at the
  cost of less precise solutions. Phase E traded speed for correctness.

**Previous results (pre-Phase-E, cut fix + pseudocost + probing, 5 seeds × 10 runs):**

| Scenario | Ralph avg | GLPK avg | Speed | Gap avg | Gap max |
|----------|-----------|----------|-------|---------|---------|
| milp15 | **1.32 ms** | 6.52 ms | **5.7x Ralph** | 25.7% | 395.8% |
| milp30 | **5.16 ms** | 7.79 ms | **1.8x Ralph** | 1.72% | 8.6% |
| milp50 | 20.69 ms | **10.39 ms** | 0.7x | 0.69% | 5.4% |
| milp75 | 102.99 ms | **50.47 ms** | 0.9x | 0.55% | 1.6% |
| milp100 | 93.58 ms | **28.80 ms** | 0.3x | 0.48% | 4.4% |
| milp200 | 751.03 ms | **129.51 ms** | 0.2x | 0.87% | 18.9% |

Multi-seed benchmarks (seeds 42, 123, 456, 789, 1337). 100% solve rate.

**Phase E impact analysis:** Phase E regressed MIP performance significantly. The old `dual_reopt`
was purpose-built for B&B: approximate DSE (weights=1.0, refine over nodes), no perturbation
(budget-limited instead), no unshift (primal values snapped directly). The replacement
`dual_simplex_solve_v2` is correct but expensive: exact DSE O(m²), perturbation O(n), unshift O(n).
For small MIPs where nodes are cheap, this overhead dominates. Options:
1. **Restore dual_reopt** — revert Phase E (loses code simplicity)
2. **Optimize v2 for warm-start** — skip perturbation/unshift when budget < 500 pivots
3. **Accept the tradeoff** — Phase E is correct, speed gaps are in MIP tree search quality not LP

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

### 1.11 Supernodal LU Factorization ✅

**Implemented** (`9df568a`, hardened `fc67b84`).

Supernodal factorization groups columns with similar sparsity structures into dense blocks
("supernodes") and uses inline BLAS kernels (dgemm/dtrsm) for inner operations. No external
BLAS dependency (WASM compatible).

**Implementation phases (all complete):**

| Phase | Task | Status |
|-------|------|--------|
| 1 | Symbolic/numeric separation | ✅ Done (T1.4, `5aaa91c`) |
| 2 | Elimination tree | ✅ Done |
| 3 | Supernode detection | ✅ Done (fundamental + relaxed merge) |
| 4 | Dense BLAS kernels | ✅ Done (inline dgemm/dtrsm for 2-20 column supernodes) |
| 5 | Supernodal triangular solves | ✅ Done |

**Hardening** (`fc67b84`): COO bounds checks, integer overflow protection, double-swap safety.

**Key design decisions:**
- No external BLAS dependency (WASM compatibility, zero-dependency mandate)
- Inline dense kernels sized for typical LP supernodes (2-20 columns)
- Hyper-sparse threshold: skip supernodal path when RHS density < 10%
- LP-specific optimization: order singleton columns (identity/slack) last, apply
  supernodal only to the non-trivial submatrix

**Files:** `src/lu.c`, `src/lu_supernode.c`
**References:** SuperLU (Demmel et al.), CHOLMOD, GLPK `bflib/`

### 1.12 State-of-the-Art LP Gap Analysis (Feb 2026)

Comprehensive comparison of Ralph's LP solver against production solvers (GLOP, CLP, GLPK).
See `docs/roadmaps/ralph_vs_glop.md` for the original GLOP comparison. This section extends
it with a full codebase audit.

**Current position (Feb 2026):** Ralph's LP solver has all standard features of a production
solver (~95% of state-of-the-art feature coverage). All Tier 1-3 gaps closed plus Phase D/E,
supernodal LU (T2.1), sparse BTRAN (W1), dual refinement (W4), runtime tolerances (W2),
two-sided perturbation (W5), SE+Devex hybrid pricing (W3). NETLIB: 22/25 pass (capri fixed).
Competitive on small-medium problems (~1x GLPK). Gap concentrated on degenerate cycling
(bandm 609x, recipe 11546x). See `docs/roadmaps/ralph-vs-glop.md` for frank assessment.

#### What Ralph Does Well

| Feature | Quality | Location | Notes |
|---------|---------|----------|-------|
| LP-aware LU factorization | Excellent | `lu_sparse.c:1630` | Separates identity/structural columns; factorizes only the k×k structural submatrix |
| Symbolic/numeric LU separation | Excellent | `lu.c`, `lu_sparse.c` | T1.4 full: symbolic analysis with fingerprint caching, arena workspace, row-major GE, O(1) identity, L/U capacity |
| Hyper-sparse FTRAN/BTRAN | Excellent | `lu.c:1345,1441` | DFS-based reach computation, 12.5% density threshold |
| FT spike pool | Very good | `lu.c` | Contiguous cache-friendly storage with offset indexing |
| Multi-round scaling | Very good | `simplex.c:249` | T1.2: geometric mean + equilibrium scaling, orthogonal to primal/dual |
| Crash basis | Very good | `simplex.c:4152` | T1.1: triangular crash for primal simplex, with singular/infeasible fallback |
| Dual simplex standalone | Very good | `dual_simplex.c` | T1.3: `dual_simplex_solve_from_scratch_v2` + `dual_phase1`, auto mode with primal fallback |
| Dual warm-start for B&B | Very good | `mip.c:solve_node_lp` | Phase E: single warm-start path via `dual_simplex_solve_v2`, with bound perturbation, exact DSE, unshift cleanup |
| Post-solve verification | Very good | `simplex.c:420` | T2.3: Ax=b, bound, dual, complementary slackness checks; OPTIMAL→IMPRECISE downgrade; IMPRECISE triggers primal fallback in method=2 |
| Phase 1 recovery | Very good | `simplex.c`, `dual_simplex.c` | Dual rescue, entering exclusion, alternate leaving, redundant row marking |
| LP Presolve | Good | `presolve.c` | 12 techniques, 20-round fixed-point, probing with implication propagation |
| Devex pricing | Good | `simplex.c:1533` | Approximate SE with periodic reference reset every 2n iterations |
| Heap pricing (T2.2) | Good | `simplex.c` | Binary max-heap with improvement scoring, incremental maintenance, bound-flip fix |
| Sparse Markowitz LU | Good | `lu_sparse.c:974` | AMD ordering, singleton detection, threshold pivoting |
| Bound flipping (P5) | Good | `dual_simplex.c` | Two-pass Harris in dual ratio test |
| DSE pricing (P6) | Good | `dual_simplex.c` | Exact init in `dual_simplex_solve_v2`, weights persist across B&B nodes |
| Objective limits | Good | `simplex.c:3899` | T3.1: early-exit in phase2 when obj exceeds limit |
| Dynamic refactorization | Good | `lu.c:1959` | T3.2: condition-based adaptive refactorization period |
| Per-phase pricing | Good | `simplex.c:4571` | T3.4: Dantzig in Phase 1, Devex in Phase 2 |
| Dual Phase 1 | Good | `dual_simplex.c:1778` | T3.5: auxiliary-objective pivots for dual feasibility |
| Bound perturbation | Good | `simplex.c:2736` | T3.3: proactive anti-cycling for primal simplex |
| Sparse BTRAN | Very good | `lu.c` | W1: DFS-based reach on U^T/L^T via CSC, density < 25% threshold |
| Dual iterative refinement | Good | `simplex.c:verify_solution` | W4: rc[j] = c[j] - A^T y recomputation when dual infeasibility detected |
| Two-sided dual perturbation | Good | `dual_simplex.c` | W5: LB + UB perturbation (LB skipped in MIP context) |
| Runtime tolerances | Good | `ralph.c` | W2: `feas_tol`, `opt_tol`, `pivot_tol` via `ralph_set_dbl_param()` |
| SE+Devex hybrid pricing | Good | `simplex.c` | W3: Devex init weights + exact SE update formula (pricing=5) |

#### Tier 1: Critical Gaps (2-5x impact each)

**T1.1 Crash Basis** — ✅ DONE

Triangular crash implemented in `simplex.c:4152` (`crash_triangular()`). Primal simplex only
(dual starts from slack basis by design — y=0, rc=c ideal for `make_dual_feasible`). Includes
singular basis fallback and post-verify infeasibility revert. Enabled via `crash=1` param.

**T1.2 Multi-Round Scaling** — ✅ DONE

Multi-round geometric mean + equilibrium scaling in `simplex.c:249` (`apply_scaling()`).
Orthogonal to primal/dual — applied in `simplex_solve` before method dispatch (line 4347).
Both paths benefit from better-conditioned matrix. Configurable via `scaling_rounds` param.

**T1.3 Dual Simplex Standalone** — ✅ DONE (not yet default)

`dual_simplex_solve_from_scratch_v2()` in `dual_simplex.c:2278` with proper `dual_phase1()`.
Auto mode (method=2) tries dual first, falls back to primal if verification fails.
NETLIB auto: 16/17 pass (brandy FIXED — perturbation backup bug). Remaining work: make dual the
default solver (Phase D), now unblocked.

**T1.4 Symbolic/Numeric Separation in LU** — ✅ DONE

Full symbolic/numeric separation with fingerprint caching. `lu_symbolic_analyze()` produces pivot
ordering and elimination tree; `lu_numeric_factorize()` reuses symbolic analysis when sparsity
pattern unchanged (fingerprint = column pointer diff). Workspace pre-allocation, arena-allocated
arrays, row-major GE, O(1) identity placement, L/U capacity tracking all included.

- **Impact**: 1.5-2x on refactorization (42% of per-iteration cost → ~25%).
- **Commits**: `5aaa91c` (full separation), `622013a` (COO pre-alloc), `32a4804` (workspace pre-alloc).
- **Dependencies**: Enables §1.11 supernodal (T2.1).

#### Tier 2: High-Impact Gaps (1.5-3x on specific scenarios)

**T2.1 Supernodal LU Factorization** — ✅ DONE

Implemented in §1.11 (`9df568a`, hardened `fc67b84`). Groups columns with similar sparsity
into dense blocks, uses inline BLAS-3 kernels (no external dependency). Reduces LU factorization
cost via cache-friendly dense operations on 2-20 column supernodes.

- **Impact**: 3-5x factorization, ~2x overall for m > 500.
- **Commits**: `9df568a` (implementation), `fc67b84` (hardening: COO bounds, overflow, swap safety).

**T2.2 Heap-Based Pricing (DynamicMaximum)** — ✅ DONE

Binary max-heap over non-basic variables keyed by improvement score (`heap_score()`). Scoring:
NONBASIC_LOWER + rc<0 → -rc, NONBASIC_UPPER + rc>0 → +rc, FREE → |rc|, else → 0 (ineligible).
Heap maintained incrementally: `heap_update()` after RC changes in `simplex_pivot()`, `heap_remove()`
for entering→basic, `heap_insert()` for leaving→non-basic. Lazy `heap_build()` after full RC
recomputation (refactorization, drift correction).

**Critical bug found/fixed:** Bound flips in `simplex_pivot()` (leaving_pos == -2) change variable
status but return early before the RC update loop where heap maintenance happens. Variables
accumulate as "zombies" at heap root with score=0 blocking eligible entries. Fix A: `heap_update()`
in bound-flip early-return path. Fix B: `pricing_heap()` pop-and-skip loop as safety net.

**Orthogonality:** Heap accelerates Dantzig's O(n) scan to O(1) extraction. NOT composable with
Devex/SE — weighted scoring (`rc/weight`) changes for ALL non-basic vars every pivot, making heap
maintenance O(n log n) which is worse than the O(n) scan it replaces. Devex remains default.

- **NETLIB**: Heap 14/17 (matches Dantzig), Devex 16/17 (unchanged).
- **API**: `ralph_set_int_param(model, "pricing", 4)`.
- **Commit**: `b3594c6`.

**T2.3 Post-Solve Verification** — ✅ DONE

`verify_solution()` in `simplex.c:420`. Checks primal feasibility (||Ax-b||), bound feasibility,
dual feasibility, complementary slackness, objective accuracy, and basis conditioning.
Downgrades OPTIMAL → IMPRECISE if any threshold exceeded. Orthogonal to primal/dual — runs
after both paths. Always-on for method=2 (auto), configurable via `verify=1` for method=0.

#### Tier 3: Moderate Gaps (10-30% improvements)

| ID | Gap | Status | Notes |
|----|-----|--------|-------|
| T3.1 | **Objective limits** | ✅ DONE | Early-exit in `simplex_phase2` when obj exceeds limit (`simplex.c:3899`) |
| T3.2 | **Dynamic refactorization** | ✅ DONE | Condition-based adaptive period (`lu.c:1959`) |
| T3.3 | **Bound perturbation** | ✅ DONE | Proactive anti-cycling (`simplex.c:2736`) |
| T3.4 | **Per-phase pricing** | ✅ DONE | Dantzig in Phase 1, Devex in Phase 2 (`simplex.c:4571`) |
| T3.5 | **Dual Phase 1** | ✅ DONE | Auxiliary-objective pivots (`dual_simplex.c:1778`) |
| T3.6 | **Basis conditioning** | ✅ DONE | Tracked in LU, used for T3.2 adaptive refactorization |

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

**Regression gate:** `make clean && make test` in ralph/ (335+ tests) AND fuelwise/ (123+ tests),
plus ASAN build (`CFLAGS="-fsanitize=address,undefined -g" make test`).

**Completed implementations** (see "What Ralph Does Well" table above for locations):
- T1.1 Crash Basis — `crash_triangular()`, primal only, with singular/infeasible fallback
- T1.2 Multi-Round Scaling — `apply_scaling()`, orthogonal to primal/dual
- T1.3 Dual Simplex Standalone — `dual_simplex_solve_from_scratch_v2()` + `dual_phase1()`
- T1.4 Symbolic/Numeric LU Separation — full separation with fingerprint caching (`5aaa91c`)
- T2.2 Heap-Based Pricing — binary max-heap, improvement scoring, bound-flip fix (`b3594c6`)
- T2.3 Post-Solve Verification — `verify_solution()`, orthogonal to primal/dual
- T3.1 Objective Limits — early-exit in phase2
- T3.2 Dynamic Refactorization — condition-based adaptive period
- T3.3 Bound Perturbation — proactive anti-cycling
- T3.4 Per-Phase Pricing — Dantzig Phase 1 / Devex Phase 2
- T3.5 Dual Phase 1 — auxiliary-objective pivots
- T3.6 Basis Conditioning — tracked in LU, drives T3.2

**Orthogonality summary:**
| Feature | Primal | Dual | Notes |
|---------|--------|------|-------|
| Scaling (T1.2) | ✅ | ✅ | Applied before method dispatch in `simplex_solve` |
| Crash (T1.1) | ✅ | ❌ | Primal only; dual needs y=0 from slack basis |
| Verify (T2.3) | ✅ | ✅ | Runs after both paths; always-on for method=2 |
| Obj limits (T3.1) | ✅ | ✅ | `objective_limit` in both primal and dual |
| Per-phase pricing (T3.4) | ✅ | N/A | Primal Phase 1/2 only |
| Perturbation (T3.3) | ✅ | separate | Primal bound perturb; dual has own dual perturbation |

**T1.4 Symbolic/Numeric LU Separation — ✅ IMPLEMENTED**

Full symbolic/numeric separation with fingerprint caching. `lu_symbolic_analyze()` produces pivot
ordering and elimination tree; `lu_numeric_factorize()` reuses symbolic when sparsity fingerprint
(column pointer diff) is unchanged. COO arrays pre-allocated. Commits: `5aaa91c`, `622013a`, `32a4804`.

**T2.2 Heap-Based Pricing — ✅ IMPLEMENTED**

Binary max-heap (pricing=4) with improvement-score keying. Heap maintained incrementally during
`simplex_pivot()`: `heap_update()` on RC changes, `heap_remove()` entering→basic, `heap_insert()`
leaving→non-basic, `heap_update()` on bound flips. Lazy `heap_build()` after full RC recomputation.
Critical bound-flip bug found/fixed (Fix A + Fix B safety net). NETLIB: 14/17 (matches Dantzig).
Not composable with Devex/SE (weighted scoring). Commit: `b3594c6`.

**T1.3 Dual Simplex as Default — ✅ ALL PHASES COMPLETE** (Phases A-E done)

Standalone dual simplex implemented and deployed as default for both LP and MIP. The old
`dual_reopt()` has been deleted and replaced by `dual_simplex_solve_v2()` warm-start in B&B.

**NETLIB status with method=2:** 16/17 pass.
- blend LP (83-var): falsely reports unbounded (benchmark data issue with duplicate entry)
- brandy: ✅ FIXED — perturbation backup corruption on re-perturbation

*Phases (all complete):*

**Phase A: Crash basis enables dual start** — ✅ DONE (T1.1)

Crash implemented for primal (method=0). Dual from-scratch uses independent tableau
(`tableau_create_dual`) without Big-M artificials — all c_B=0 gives y=0, rc=c.

**Phase B: Clean dual solver function** — ✅ DONE

`dual_simplex_solve_from_scratch_v2()` with `dual_phase1()`, `make_dual_feasible()`,
`dual_simplex_solve_v2()`. No primal fallback — caller decides.

**Phase C: Method dispatch in simplex_solve** — ✅ DONE

Method dispatch: method=0 (primal), method=1 (dual forced), method=2 (auto: dual first,
verify, primal fallback if dual fails or IMPRECISE).

**Phase D: Make dual the default** — ✅ DONE (`dac309a`)

Changed default `method` from 0 to 2. All tests pass. NETLIB: 16/17.

**Phase E: Replace dual_reopt in B&B** — ✅ DONE (`140a1f2`)

Collapsed `solve_node_lp()` from 3-path dispatch (PATH A/B/C with `dual_reopt`) to single
warm-start path using `dual_simplex_solve_v2()`. MIP now uses method=2 instead of method=0.

Code deleted: `dual_reopt()` (~205 LoC), old `dual_simplex_solve()` (~340 LoC), old
`dual_simplex_solve_from_scratch()` (~290 LoC), `dual_ratio_test_bflip()`, `dse_init_approx()`,
`restore_basis_from_node()`. Net: +113 -1237 lines across 5 files.

Bugs found and fixed during Phase E:
1. **Non-basic variable unshift**: After `remove_bound_perturbation`, non-basic vars at UB
   still held perturbed x values (e.g., 1+ε instead of 1). Fix: snap to restored bounds.
2. **verify_solution obj_sense**: Recomputed objective was multiplied by `model->obj_sense`,
   causing 2x relative error on maximization problems. Latent bug exposed by method=2
   forced verification in MIP. Fix: removed spurious multiplication.
3. **IMPRECISE primal fallback**: method=2 now falls back to primal when `verify_solution`
   downgrades to IMPRECISE (was returning bad dual solution on ill-conditioned subproblems).

All tests pass: Ralph 359/359, LAP 358/358, Netflow 153/153, FuelWise 123/123.

**Phase E Performance Fix** (`f80487e`):

Phase E caused catastrophic MIP regression (milp15: ~1ms → 92s). Three root causes:
1. `dse_init_exact()` (O(m²)) called unconditionally every v2 call. Fix: conditional on
   `!tab->dse_initialized`; callers invalidate explicitly when basis changes significantly.
2. No iteration budget on v2 calls in diving/RINS/node-solve. Default 1M iterations let
   stall-detection-fooling cycling burn minutes. Fix: 500-iter cap in `mip_apply_dual_flags()`
   + explicit save/restore at each call site.
3. Cold-start `simplex_solve` with method=2 wasted 500 iterations retrying dual (which already
   failed). Fix: use method=0 (primal) for cold-start paths.

Post-fix: milp15 10-23ms avg (24/25 solved), all tests pass. But overall MIP speed is still
worse than pre-Phase-E due to v2 overhead (exact DSE, perturbation, unshift per warm start).

#### Implementation Order (Prioritized by Impact/Effort)

| Order | ID | Feature | Impact | Effort | Deps | Flag |
|-------|-----|---------|--------|--------|------|------|
| 1 | T1.2 | Multi-round equilibrium scaling | Better numerics, may fix blend | ~100 LoC | None | `scaling_rounds` |
| 2 | T1.1 | Triangular crash basis | 2-5x cold starts | ~300 LoC | None | `crash` |
| 3 | T2.3 | Post-solve verification | Correctness | ~150 LoC | None | `verify` |
| 4 | T3.1 | Objective limits in simplex_solve | 30-50% fewer pruned-node iters | ~50 LoC | None | `obj_limit` |
| 5 | T3.4 | Per-phase pricing strategy | 10-15% Phase 1 improvement | ~30 LoC | None | `phase1_pricing` |
| 6 | T3.6 | Basis conditioning report | Diagnostics | ~30 LoC | None | `verify` (shared) |
| 7 | T1.4 | Symbolic/numeric LU separation | 1.5-2x refactorization | ~500 LoC | None | `lu_symbolic_reuse` | ✅ DONE |
| 8 | T2.2 | Heap-based pricing | 2-5x pricing on large problems | ~200 LoC | None | `pricing=4` | ✅ DONE |
| 9 | T3.2 | Dynamic refactorization period | 10-20% LU amortization | ~50 LoC | None | `refac_dynamic` |
| 10 | T3.3 | Cost perturbation | Fewer degenerate pivots | ~80 LoC | None | `cost_perturb` |
| 11 | T2.1 | Supernodal LU (§1.11) | 3-5x factorization | ~1500 LoC | #7 (T1.4) | `lu_supernode` |
| 12 | T3.5 | Dual Phase 1 with auxiliary objective | Robust dual starts | ~200 LoC | None | `dual_phase1` |
| 13 | T1.3 | Dual simplex as default | ~2x initial solves | ~400 LoC | #2 (T1.1), P5, P6 | `method` | ✅ DONE |
| 14 | T1.3e | Replace dual_reopt in B&B | Consistent node LP quality, simpler MIP solver | -1124 LoC (net delete) | #13 (T1.3) | N/A (removes code) | ✅ DONE |

All Tier 1-3 items plus supernodal LU (T2.1) and LP gap-closing work items (W1-W5) are
complete (~95% of state-of-the-art feature coverage).

**Milestone targets:**

| Milestone | Requirements | Expected Result |
|-----------|-------------|-----------------|
| **75% SotA** | T1.2, T1.1, T2.3, T3.1, T3.4, T3.6 | ✅ REACHED — blend NETLIB passes, 2-3x cold start improvement, numerical diagnostics |
| **85% SotA** | + T1.4, T2.2, T3.2, T3.3 | ✅ REACHED — Competitive per-iteration speed on m < 2000, proper LU reuse, heap pricing |
| **90% SotA** | + T1.3 (dual-as-default) | ✅ REACHED — Dual simplex default, competitive with GLPK/CLP on NETLIB |
| **92% SotA** | + T1.3e (replace dual_reopt) | ✅ REACHED — Clean MIP node solver, single warm-start path, -1124 LoC |
| **95% SotA** | + T2.1 + W1-W5 | ✅ REACHED — Supernodal LU, sparse BTRAN, dual refinement, runtime tolerances, SE+Devex, 22/25 NETLIB |

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
| **Objective cutoff** | ✅ **Done** | 1.1-1.4x on small MIP | Prune nodes via `objective_limit` when obj exceeds incumbent |
| **c-MIR cuts** | ✅ **Done** | Tighter relaxation | Row normalization sign fix in GMI/c-MIR back-substitution; safety guard for cut-induced infeasibility |
| **Bound flipping (P5)** | ✅ **Done** | 20-50% on small MIP | Flip boxed vars in dual ratio test |
| **Dual steepest edge (P6)** | ✅ **Done** | 20-50% on small MIP | DSE leaving selection; exact init in `dual_simplex_solve_v2` |
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

---

## Chapter 8: LP Performance Improvement Plan (Feb 2026)

Based on deep analysis of `simplex.c` (4865 LoC), `dual_simplex.c` (1546 LoC), `lu.c` (2050 LoC),
`sparse.c` (648 LoC), and full NETLIB test results (84 problems, 5 tiers).

### 8.1 Current State

**NETLIB Correctness (Tier 0-1, fast):** 22/25 PASS, 2 SKIP (bore3d, scagr25), 1 ERROR (share1b)
**beaconfd:** Excluded (tier 5, Phase 2 degenerate pivot failure)

Post W1-W5 (`929afe5`): capri now solves (was timeout). All pricing strategies (Devex/SE+Devex/SE)
produce identical 22/25 results.

| Problem | Status | Cause |
|---------|--------|-------|
| beaconfd | INFEASIBLE (false) | Phase 2 pivot failure: ratio test selects theta=0 leaving with near-zero pivot element |
| bore3d | Timeout | Cycling |
| scagr25 | Timeout | Large degenerate |
| share1b | ITERATION_LIMIT | Exceeds iter cap at 53s |

**Performance:** ~1x GLPK on small (m<100), 3-15x on medium (m=100-500), catastrophic on
cycling problems (bandm 609x, recipe 11546x). Overall competitive for embedded use case.

### 8.2 Identified Bottlenecks

#### B1. Redundant `tableau_compute_solution()` every dual iteration
- **File:** `dual_simplex.c:873`
- Dual pivot at lines 330-332 incrementally updates `x_B -= step * d`
- Then line 873 recomputes the entire solution from scratch via FTRAN
- CLP/GLPK rely on incremental update, recompute only after refactorization
- **Impact:** ~20-30% of dual simplex time. **Fix:** ~5 lines

#### B2. `dual_simplex_pivot()` mallocs 5 arrays per pivot
- **File:** `dual_simplex.c:247-266`
- 5 malloc + 5 memcpy + 5 free per pivot for rollback-on-failure
- 5000 pivots = 25,000 malloc/free pairs of large arrays
- **Impact:** ~5-15%. **Fix:** ~30 lines (pre-allocate in tableau)

#### B3. Devex paying full Steepest Edge cost
- **File:** `simplex.c:2540-2546`
- Every primal pivot does 1 extra BTRAN + n sparse dot products for SE weights
- Applied even when pricing_strategy=Devex, defeating Devex's purpose
- Comment: "Devex approximation leads to worse pivot selection"
- **Impact:** 2-3x slowdown on primal. **Fix:** ~50 lines

#### B4. FT spike compaction is O(m^2 * N)
- **File:** `lu.c:1612-1666`
- After 300 spikes, computes dense m x m product matrix
- For m=500: 75M ops, then O(m^2) per subsequent FTRAN/BTRAN
- Destroys sparse LU benefits for medium/large problems
- **Impact:** Makes tier 2+ unusably slow. **Fix:** ~20 lines (delete + tune refactor threshold)

#### B5. `dse_init_exact()` is O(m^2) per refactorization
- **File:** `dual_simplex.c:222-231`
- m BTRAN solves to initialize DSE weights exactly
- Called on every refactorization in dual simplex
- **Impact:** 2-5x on dual for medium+ problems. **Fix:** ~20 lines

#### B6. `verify_solution()` is O(n*m) instead of O(nnz)
- **File:** `simplex.c:436-446`
- Triple-nested loop scanning all columns for each row
- For m=1000, n=2000: ~10M ops vs ~10K with sparse matvec
- **Impact:** 100x on verify (small total, but big for MIP). **Fix:** 10 lines

#### B7. No row-form (CSR) for RC update
- Both `simplex.c:2704-2730` and `dual_simplex.c:297-310`
- O(n) sparse dot products per pivot for RC update
- CSR copy enables row-scatter with better cache behavior
- **Impact:** 1.5-3x on RC kernel (~40% of pivot time). **Fix:** ~300 lines

### 8.3 Implementation Plan

#### Week 1: Low-Hanging Fruit — ✅ DONE (`a67f09f`)
1. ✅ Remove per-iteration `tableau_compute_solution` in dual_v2 (B1)
2. ✅ Pre-allocate backup arrays in `dual_simplex_pivot` (B2)
3. ✅ Delete FT spike compaction + tune refactorization threshold (B4)
4. ✅ Approximate DSE init — weights=1.0, exact only on first factorization (B5)
5. ✅ Fix `verify_solution` to O(nnz) sparse matvec (B6)

**Result:** -138 LoC, zero regressions. Negligible speedup on tier 0-1 (~1%) because
the hot path is primal simplex (Devex pricing + FTRAN/BTRAN), not dual. These fixes
clean up dead code paths and eliminate per-pivot allocations for when dual is used
heavily (MIP warm-starts, large dual-preferred LPs).

#### Week 2: Devex/SE Fix — ✅ DONE
6. ✅ Proper Devex weight formula without tau BTRAN (B3)
   - Phase 1 (artificials in basis): uses exact SE (tau BTRAN) for accuracy
   - Phase 2 (all artificials out): uses Devex approximation (Harris 1973)
   - Devex formula: `w_j' = max(0.999 * w_j, (alpha_j/pivot)^2 * gamma_e)`
   - Saves O(m^2) BTRAN per pivot in Phase 2 (bulk of iterations)

**Result:** 1-4% speedup on tier 1, 2-4% on tier 2 (except bnl1 -9% due to
extra iterations from approximate weights). Modest because true SE was already
integrated into the merged RC/weight update loop — the BTRAN is only one of
several O(m) operations per pivot. Bigger gains require Supernodal LU (T2.1).

#### Week 3: Phase 1 Robustness — PARTIAL
7. Two-phase simplex and adaptive Big-M investigated but not viable:
   - Universal two-phase: breaks beaconfd, lotfi (Phase 1→2 transition fragile)
   - Adaptive Big-M (1e4 * max|c| * max|b|): doesn't help kb2/recipe
   - Root cause for kb2/recipe: likely dual simplex (method=2) returning wrong answer,
     not a Big-M/Phase 1 issue. Needs deeper investigation.
7a. ✅ Fixed partial pricing + two-phase interaction (stale RC bug):
   - Phase 1 pricing override: partial/heap → Devex for two-phase Phase 1
   - Phase 2 full RC recomputation after two-phase transition
   - `tab->pricing_strategy` and `tab->use_steepest_edge` saved/restored with `solver->pricing_strategy`
8. forplan MPS parsing (integer markers in COLUMNS section) — not yet implemented

#### Week 4: Row-Form RC Update (1.5-3x, ~300 lines) — TODO
9. Build CSR copy of A_ext at tableau creation (B7)
10. Row-scatter RC update in both primal and dual pivot

#### Supernodal LU (T2.1) — ✅ DONE (`9df568a`, `fc67b84`)
- BLAS-3 dense blocks within sparse structure
- Inline dgemm/dtrsm for 2-20 column supernodes
- No external BLAS dependency

#### LP Gap Closure (W1-W5) — ✅ DONE (`929afe5`)
- W1: Sparse BTRAN — DFS-based reach on U^T/L^T (~330 LoC in lu.c)
- W2: Runtime tolerances — `feas_tol`, `opt_tol`, `pivot_tol` via API
- W3: SE+Devex hybrid pricing — pricing=5 (Devex init + exact SE update)
- W4: Dual iterative refinement — rc recomputation in verify_solution
- W5: Two-sided dual perturbation — LB perturbation (LP only, skipped in MIP)

### 8.4 Current Outcome

**Achieved (Feb 2026, post-Markowitz + LP-perf):**
- 22/22 NETLIB fast-tier PASS (100%), 34/36 solvable across all tiers
- ~1x GLPK on small problems (m < 100)
- 2-3x GLPK on medium problems (israel, brandy)
- 10-20x on large (bandm, scorpion)
- czprob 16% faster with supernodal + conservative refactorization

**Implemented (Feb 2026):**
- CSR row-scatter RC update (density < 2%, for sparse problems)
- Supernodal LU auto-enabled for m > 300
- Conservative refactorization thresholds for m ≥ 500 (100 updates, spike-work trigger)

**Remaining gaps:**
- Ratio test pivot threshold for beaconfd (~30 LoC)
- Batched spike application (5-15% solve, ~200 LoC)
- External BLAS for large supernodes (currently inline-only)

### 8.5 LU Factorization: State-of-the-Art Assessment (Feb 2026)

Ralph's LU is algorithmically sound but computationally scalar. It implements the right
algorithms at ~40% of production solver throughput due to missing vectorized operations.

#### What Ralph Has

| Feature | Quality | Notes |
|---------|---------|-------|
| AMD pivot ordering | Good | Proper elimination graph, element absorption, degree lists |
| LP-specific column ordering | Excellent | Identity/slack detection, factorizes only k×k structural submatrix |
| Symbolic/numeric separation | Excellent | T1.4: fingerprint-cached symbolic analysis, arena workspace |
| Sparse Markowitz pivoting | Good | Threshold 0.1, singleton detection |
| Hyper-sparse FTRAN/BTRAN | Excellent | DFS-based reach computation, 12.5% density threshold |
| FT spike pool | Very good | Contiguous cache-friendly storage, offset indexing |
| Growth monitoring | Good | Refactorize at growth > 1e8, condition κ from U diagonal |
| Threshold pivoting in updates | Good | |pivot| ≥ 0.001 × max|spike|, forces refactorize on violation |
| Pre-allocated workspaces | Good | Dense m×m workspace, COO arrays, sparse index arrays |

#### What Ralph Lacks

| Feature | Impact | Effort | Notes |
|---------|--------|--------|-------|
| ~~Supernodal LU~~ | ~~3-5x factorize~~ | ~~done~~ | ✅ Implemented + auto-enabled for m > 300 |
| **Batched spike application** | 5-15% solve | ~200 LoC | Apply multiple FT spikes as BLAS-2 instead of scalar loops |
| **Fill-reducing column sort** | 10-20% fill | ~150 LoC | Currently disabled (beaconfd/lotfi regression); needs stability guards |
| **Robust regularization** | Correctness | ~100 LoC | Implemented but disabled; tiny diagonals → NaN, large ones → UNBOUNDED |
| **Nested dissection ordering** | 5-10% fill | ~300 LoC | Provably tighter fill bounds than AMD; low ROI for Ralph's problem sizes |

#### Comparison Matrix

```
                          RALPH    GLPK     CLP      GLOP
Pivot ordering            AMD      AMD      ND       ND
Supernodal LU             ✓        ✓        ✓✓       ✓✓     ← implemented (inline BLAS)
Symbolic/numeric sep.     ✓        ✓        ✓        ✓
Hyper-sparse solve        ✓        ✓        ✓        ✓
Sparse BTRAN              ✓        ✓        ✓        ✓      ← W1 (DFS reach on U^T/L^T)
Batched spike apply       ✗        ✗        ✓        ✓      ← 5-15%
Dense BLAS kernels        ✓(inline)✗        ✓(ext)   ✓(ext) ← supernodal, no ext dep
Growth monitoring         1e8      1e12     1e10     1e10
Threshold pivoting        0.1      0.1      0.1      0.1
Regularization            disabled ✓        ✓        ✓      ← correctness
FT/eta-file updates       FT       FT       FT/BG    FT/PF
```

**Bottom line:** Ralph's LU is now a capable Tier-3/4 implementation with supernodal factorization
(auto-enabled for m > 300), hyper-sparse FTRAN/BTRAN, sparse BTRAN (W1), and symbolic/numeric
separation with fingerprint caching. The inline BLAS kernels (no external dependency) handle 2-20
column supernodes efficiently. Remaining gap to CLP/GLOP: external BLAS for large supernodes,
batched spike application, and nested dissection ordering. For Ralph's target use (embedded solver,
m < 500, WASM), this is competitive with GLPK on non-degenerate problems.

### 8.6 Post-Markowitz LP Improvement (Feb 2026)

Three changes to close the LP performance gap with GLPK on large problems:

**Phase 1: Conservative Refactorization (lu.c)**
- `max_updates = 100` for m ≥ 500 (was 200) — 2x more frequent, trades fast Markowitz for cheaper spike application
- Spike-work trigger: refactorize when `spike_pool_used > m * 8` (for m ≥ 500 only)
- m < 500 unchanged (50-m/2) to preserve MIP warm-start behavior

**Phase 2: CSR Row-Scatter RC Update (simplex.c)**
- Build CSR transpose of A_ext at tableau creation (one-time O(nnz))
- Row-scatter: accumulate alpha_j from pivot_row non-zeros instead of scanning all n columns
- Only enabled when matrix density < 2% (column-scan benefits from SIMD vectorization at higher density)
- For sparse problems (czprob density 0.4%): 10-20x less arithmetic in RC kernel

**Phase 3: Supernodal LU Auto-Enable (simplex.c, dual_simplex.c)**
- Enable supernodal LU by default when m > 300 (was opt-in only)
- Supernodal factorization 2-5x faster via BLAS-3 dense blocks on structural columns
- m > 300 threshold avoids overhead for medium problems where supernodal try+fail adds latency

**Results (NETLIB A/B comparison):**

| Problem | m | Baseline | After | Change |
|---------|---|----------|-------|--------|
| czprob | 929 | 23.2s | 19.5s | **-16%** |
| fit1p | 627 | 5.5s | 5.6s | same |
| scfxm1 | 330 | 169ms | 160ms | -5% |
| bandm | 305 | 346ms | 347ms | same |
| brandy | 220 | 35ms | 34ms | same |
| e226 | 223 | 55ms | 53ms | -4% |

No regressions on any NETLIB problem. All 378 Ralph + 123 FuelWise tests pass.

**Key lesson:** Aggressive refactorization (m/4 max_updates) helps large problems but HURTS medium
problems where Markowitz factorization cost exceeds spike application savings. The threshold m ≥ 500
preserves existing performance while enabling gains on truly large problems. Similarly, supernodal
LU at m > 300 is a net win but m > 150 adds overhead for e226-class problems (m~220).

### 8.7 Unified Pressure-Based Periodic Refactor Scheduler (Feb 2026)

Replaced the split "legacy large-basis guardrail + adaptive health skip" with a single scheduler
in `simplex.c` that computes one pressure signal and one periodic plan for both Phase 1 and Phase 2.

**Signals used:**
- `size_pressure`: ramps from 0 to 1 between `m=350` and `m=500` (large bases stay conservative)
- `health_pressure`: weighted LU health (`degeneracy/Bland`, spike-pool usage, condition estimate, growth)
- `update_pressure`: `num_updates / max_updates` (forces periodic refresh as update chains age)

**How interval is chosen:**
1. Start from legacy phase base interval (`max_updates/3` in phase 1, `max_updates/4` in phase 2,
   clamped to phase min/max).
2. Tighten base interval toward phase minimum using `size_pressure`.
3. Relax only a bounded portion of the remaining range (1/3 span) when pressure is low.
4. Use `run_pressure = max(interval_pressure, update_pressure)` to gate actual execution.

**When periodic refactor runs:**
- `num_updates` must satisfy age and cadence checks (`min_update_age`, multiple of interval).
- Under strong cycling (`use_bland` or high degeneracy), periodic refactor is always eligible.
- Otherwise it runs when `run_pressure >= 0.40`.

This keeps large two-phase NETLIB cases (`fit1p`, `nesm`) on conservative periodic cadence while
still allowing medium/small cases to skip unnecessary refactors when LU health is stable.

### 8.8 LP Refactor Performance Plan (Iterative, Reusable)

Current bottleneck on large degenerate LPs is no longer sparse→dense fallback; it is full
refactor wall-time plus frequent periodic reinversion (`fit1p`, `nesm`, `scagr25` class).
Track execution with these phases:

1. ✅ **Stage-level refactor telemetry** (done in `5ad46a9`)
   Add timers/counters for basis rebuild, symbolic analyze, sparse numeric, dense numeric,
   and internal sparse→dense fallback reasons.
2. ✅ **Remove avoidable fallback/copies** (done in `59958a4`)
   Eliminated symbolic malloc churn and enabled sparse symbolic `k=m` viable path, with
   sparse numeric hardening for identity-separation edge cases.
3. ✅ **Periodic scheduler effectiveness feedback** (done)
   Kept LU hard safety triggers and added per-phase adaptive bias that relaxes/tightens
   periodic cadence from observed periodic-refactor outcomes.
4. ✅ **Cheaper full refactor path** (done)
   Added incremental basis-matrix maintenance fast-paths for refactor extraction:
   unchanged layout patching and span-rewrite+tail-shift when nnz layout changes,
   with safe full-rebuild fallback.
5. ✅ **Faster long FT-chain solves** (done)
   Added batched/cache-optimized FT spike micro-kernels for FTRAN/BTRAN
   with preserved spike-application order.
6. ▶ **Hard regression discipline** (next)
   Every step must pass `make -C ralph test`, `make -C ralph test-netlib-gate`,
   and canary subset `fit1p|nesm|bandm|scagr25`.

Progress update (2026-02-20):
- Stage-level telemetry v1 is now wired in LU internals and benchmark JSON:
  symbolic calls/cache hit-miss, sparse numeric stage split (Markowitz/supernode/GE),
  dense-factorization timing, and sparse→dense fallback reason counters
  (`small_matrix`, `symbolic`, `numeric`).
- Removed the strict `num_identity >= m/4` symbolic gate so sparse LU remains eligible
  on low-identity bases (`bandm`, `scagr25` class), eliminating avoidable symbolic fallbacks.
- Removed symbolic per-refactor malloc/free churn via persistent LU workspaces and added
  sparse symbolic `k=m` fast-path so full-structural bases stay on sparse path when viable.
- Added adaptive periodic-scheduler feedback in primal phase loops: periodic interval pressure
  now includes a bounded per-phase bias learned from prior periodic effectiveness signals,
  while all hard LU safety triggers remain authoritative.
- Added first incremental basis-maintenance fast path in `simplex.c`: when basis-position
  replacements preserve per-column nnz, refactor extraction patches only changed columns in
  `basis_work` and reuses cached CSC layout; falls back to full rebuild on layout changes.
- Extended incremental basis maintenance for nnz-layout changes: refactor extraction now
  rewrites only the changed basis span and shifts trailing CSC payload in-place when capacity
  allows, preserving full-rebuild fallback for safety.
- Added batched FT spike application kernels in `lu.c` (paired spike batching plus
  inner-loop unrolled scatter/gather), reducing long-chain FTRAN/BTRAN
  overhead without changing spike execution order.

### 8.9 NETLIB Small-Canary Coverage

Added explicit NETLIB small-canary coverage (tiers 0-1 plus `beaconfd`) to keep recurrent
performance/correctness checks focused and reproducible:

- Canary allowlist file: `ralph/benchmarks/netlib_small_canary.txt` (26 problems)
- Dedicated target: `make -C ralph test-netlib-gate-small`
- Full gate baseline now enforces required small-instance presence via
  `required_coverage` in `ralph/benchmarks/netlib_regression_baseline.json`

### 8.10 Ranked Per-Iteration Hotspot Queue (Baseline `eefe861`)

Source dataset: full NETLIB gate artifacts at
`/tmp/netlib-regression-gate-20260220-121147` (84 files, 55 comparable optimal LPs).

Observed root cause split on GLPK-faster comparable cases (37):
- Per-iteration dominant only: 22 cases
- Both iteration count and per-iteration: 11 cases
- Iteration count dominant only: 4 cases
- Dense fallback contribution: 0 cases (`sparse_dense_fallbacks = 0` across solved set)

Ranked queue (per-iteration hotspots only):

1. `H1` Triangular solve micro-kernel upgrade (highest per-iter skew)
Focus set: `scfxm3`, `ganges`, `scfxm2`, `ship12l`, `ship12s`, `sctap3`, `seba`, `shell`, `etamacro`.
Evidence: iteration ratios are often < 1 while per-iteration ratios are 9x-33x.
Implementation: optimize sparse/dense triangular kernels (`solve_L*`, `solve_U*`, sparse FTRAN/BTRAN paths) with branch-light inner loops and cache-local batching.
Success gate: reduce per-iteration ratio on `scfxm3` and `ganges` by at least 30% with no objective/status regressions.

2. `H2` Full refactor wall-time reduction on large degenerate LPs
Focus set: `25fv47`, `fit1p`, `80bau3b`, `nesm`, `czprob`.
Evidence: refactor dominates core time on these cases (roughly 59%-86% of refactor+pivot+ratio+pricing).
Implementation: extend incremental basis extraction/rebuild fast paths to reduce full CSC payload rewrites and avoid avoidable workspace clears/rebuilds.
Success gate: reduce `refactor.all_ms` by at least 25% on `fit1p` and `80bau3b`; keep NETLIB gate parity.

3. `H3` Markowitz retry pressure reduction inside sparse numeric refactor
Focus set: `25fv47` (very high retry count), `80bau3b`, `nesm`, `fit1p`.
Evidence: retry counts are elevated on slow-degenerate cases and correlate with high refactor wall-time.
Implementation: improve retry policy and pivot candidate acceptance under reserved-row constraints without triggering dense fallback.
Success gate: reduce `mkz_retry_count` by at least 50% on `25fv47` and at least 30% on `80bau3b`.

4. `H4` Pivot/ratio kernel throughput improvements in simplex loop
Focus set: `25fv47`, `scfxm3`, `nesm`, `80bau3b`.
Evidence: pivot+ratio remains a large secondary block after refactor in top slow cases.
Implementation: tighten sparse gather/scatter paths in pivot update and ratio test loops to reduce per-pivot scalar overhead.
Success gate: reduce combined `pivot_ms + ratio_ms` by at least 20% on `scfxm3` and `nesm`.

5. `H5` Pricing kernel cost on large-degenerate workload
Focus set: `80bau3b`, `25fv47`, `czprob`.
Evidence: pricing time is a meaningful tail on degenerate long runs (not the primary bottleneck but still material).
Implementation: refine pricing scan cadence/refresh behavior for large sparse degenerate bases while preserving pivot quality.
Success gate: reduce `pricing_ms` by at least 30% on `80bau3b` without increasing iteration count by more than 10%.

### 8.11 No-Regression Improvement Plan (Next Iterations)

Goal: close the remaining GLPK gap by reducing (a) reinversion/refactor pressure and (b) per-iteration
kernel cost, while preserving full NETLIB status/objective parity.

Guardrails (must pass on every change):
- `make -C ralph test`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`
- Full NETLIB GLPK comparison (`--glpk`) with zero new status/objective/invalid-solution mismatches
- Dense fallback files must remain `0` on solved comparable set

Acceptance policy:
- Promote only if geometric mean `Ralph/GLPK` time ratio improves or is neutral within noise,
  and no required canary regresses by more than 10% wall time (`fit1p`, `nesm`, `bandm`, `scagr25`, `degen3`).
- Any correctness mismatch or new timeout in previously passing canaries is a hard reject.
- Tune one lever at a time (single-feature commits) to preserve attribution.

Execution tracks (ordered):

1. Refactor pressure control (frequency, not safety)
   - Keep LU hard-safety refactor triggers authoritative.
   - Tighten policy-only periodic reinversion with bounded cooldown and pressure decay in long degenerate runs.
   - Separate telemetry counters: `policy_periodic`, `health_forced`, `safety_forced`.
   - Target: lower policy-driven refactors on `degen3`/`fit1p` without increasing fallback or instability.

2. Refactor wall-time reduction
   - Extend incremental basis extraction fast paths (span rewrite + tail shift) to more layout-change patterns.
   - Reduce avoidable clears/rebuilds in refactor staging buffers.
   - Target: `refactor.all_ms` down >=20% on `fit1p` and `80bau3b`.

3. Triangular solve kernel throughput
   - Optimize hot sparse triangular paths (`solve_L*`, `solve_U*`, sparse FTRAN/BTRAN apply loops) with
     branch-light inner loops and cache-local batching.
   - Target: per-iteration time down >=20% on `scfxm3`/`ganges` class with stable iteration counts.

4. Degeneracy stabilization without over-refactor
   - Prefer bounded anti-degeneracy actions (short Bland hold + conservative perturb) before policy periodic reinvert.
   - Keep explicit attempt caps to avoid long-tail stalls.
   - Target: reduce periodic-policy refactor share on `degen3` while maintaining objective/status parity.

5. Pricing-tail cleanup
   - Continue adaptive pricing refresh tuning only after tracks 1-4 stabilize.
   - Target: `pricing_ms` down >=20% on `80bau3b` with <=10% iteration drift.

Operational cadence:
- Run focused A/B first (`fit1p`, `nesm`, `degen3`, `bandm`, `scagr25`, plus one per-iter hotspot).
- If focused pass, run `test-netlib-gate-small`.
- If small gate pass, run full `test-netlib-gate`.
- If full gate pass, refresh GLPK comparison and update baseline section with commit hash + artifact path.

Progress update (2026-02-22):
- Track 2 instrumentation landed for basis extraction in refactor path:
  `basis_fastpath_hits`, `basis_cols_rewritten`, `basis_tail_shift_bytes`
  (exported in benchmark JSON under `refactor`).
- Added guarded incremental hook in `build_basis_matrix` for layout-change
  handling; currently kept conservative (`use_sparse_patch = 0`) to preserve
  no-regression behavior while telemetry informs the next tuning pass.
- Gates on this state: `test-simplex-policy` PASS (20/20),
  `test-lu-markowitz` PASS (59/59),
  `test-netlib-gate-small` PASS (`/tmp/netlib-regression-gate-20260222-090821`),
  full `test-netlib-gate` PASS (`/tmp/netlib-regression-gate-20260222-091008`).
