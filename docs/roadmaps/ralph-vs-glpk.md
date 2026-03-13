# Ralph vs GLPK: Control/Policy Convergence Plan

## Purpose

This roadmap defines a strict GLPK-compatible LP control policy surface for Ralph.
Goal: match GLPK control semantics (SMCP/BFCP style knobs) while preserving Ralph's
numerical safety triggers and existing no-regression gates.

Execution tracking for the current strict three-step implementation is maintained in:
- `docs/roadmaps/ralph-glpk-strict-execution.md`
- `docs/roadmaps/ralph_glpk_strict_state.json`
- `ralph/benchmarks/glpk_strict_plan.sh`

## Scope

This plan is about control/policy behavior, not replacing Ralph kernels with GLPK.
It targets:
- simplex control semantics (`method`, `pricing`, `ratio`, `flip`, `basis`, `presolve`)
- basis/factorization policy semantics (`backend`, update limit, pivot/growth guards)
- deterministic runtime mapping from params -> solver decisions

## Constraints

- Hard numerical safety triggers in LU/simplex always override policy.
- No regressions on NETLIB gates or correctness tests.
- Policy implementation must be orthogonal and unit-testable.

## Current Gap (Observed)

- Ralph has fragmented local heuristics (phase-1/phase-2/refactor/LU backend)
  with partial context.
- GLPK exposes a coherent control plane (SMCP/BFCP), which avoids local-policy
  drift and makes behavior predictable across difficult families (`pilot*`, `stair`).

## Target API/Behavior

Add GLPK-compatible policy params (explicit, runtime-settable):

1. Profile-level
- `lp_policy_profile`: `default` (0), `glpk_compat` (1)

2. SMCP-like controls
- `glpk_smcp_method`: `auto` (0), `primal` (1), `dualp` (2), `dual` (3)
- `glpk_smcp_pricing`: `standard` (0), `steep` (1)
- `glpk_smcp_ratio`: `standard` (0), `harris` (1)
- `glpk_smcp_flip`: `off` (0), `on` (1)
- `glpk_smcp_basis`: `adv` (0), `std` (1), `bib` (2), `ini` (3)
- `glpk_smcp_presolve`: `auto` (0), `off` (1), `on` (2)

3. BFCP-like controls
- `glpk_bfcp_backend`: `luf_ft` (0), `cbg` (1), `cgr` (2)
- `glpk_bfcp_update_limit`: integer (`-1` = auto/default)
- `glpk_bfcp_pivot_limit`: integer (`-1` = auto/default)
- `glpk_bfcp_suhl`: `auto` (`-1`), `off` (`0`), `on` (`1`)
- `glpk_bfcp_pivot_tol`: double (`<=0` = auto/default)
- `glpk_bfcp_eps_tol`: double (`<=0` = auto/default)
- `glpk_bfcp_growth_guard`: double (`<=0` = auto/default)
- `glpk_bfcp_nfs_max`: integer (`-1` = auto/default)
- `glpk_bfcp_nrs_max`: integer (`-1` = auto/default)

## Implementation Phases

### P0: Parameter + Profile Scaffolding (Step 1)

Deliverables:
- Extend typed parameter IDs and metadata with the knobs above.
- Add model-level storage for all new policy knobs.
- Add profile application semantics:
  - when `lp_policy_profile=glpk_compat`, populate SMCP/BFCP knobs with
    GLPK-compatible defaults.
  - explicit knob sets after profile selection override profile defaults.

Acceptance:
- Parameter metadata introspection includes all new knobs.
- Set/get APIs work for string and typed-id paths.

### P1: Coherent Policy Module (Step 2)

Deliverables:
- New orthogonal module:
  - `ralph/include/lp_policy_glpk_compat.h`
  - `ralph/src/lp_policy_glpk_compat.c`
- Module responsibilities:
  - validate policy knobs
  - normalize profile defaults
  - compute effective runtime mapping for current solver hooks
    (`method`, `pricing`, `phase1 pricing`, presolve mode, LU control hints)

Acceptance:
- `ralph.c` uses policy module as single source of truth for effective controls.
- Existing solver behavior remains unchanged under `lp_policy_profile=default`.

### P2: Runtime Hook Wiring (Done)

Deliverables:
- Replace ad-hoc phase control branches with policy-gated decisions:
  - pricing mode selection
  - ratio-test mode selection
  - flip enablement
  - basis init selection
- Hook BFCP controls into LU runtime:
  - backend preference ordering
  - update-limit override
  - pivot/growth threshold override

Acceptance:
- GLPK-compat profile can be selected end-to-end from public params.
- Hard LU safety triggers still preempt policy.

### P3: Tests + Gates (Done for current GLPK-compat slice)

Unit tests (orthogonal):
- new: `ralph/tests/test_lp_policy_glpk_compat.c`
- update: `ralph/tests/test_simplex_policy.c`
- update: `ralph/tests/test_lu_markowitz.c`

Regression gates:
- `make -C ralph test-simplex-policy`
- `make -C ralph test-lu-markowitz`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

Focused parity checks:
- `pilot.ja.mps`, `pilot.mps`, `stair.mps` with GLPK toggle comparisons:
  - `--method {0,1,2}` (primal/dual/auto)
  - `--steep | --nosteep`
  - `--relax | --norelax`
  - `--flip | --noflip`

### P3 Findings (2026-03-01)

Implemented benchmark CLI toggles in `ralph-benchmark`:
- `--steep`, `--nosteep` (pricing aliases)
- `--relax`, `--norelax` (GLPK ratio mode: Harris vs standard)
- `--flip`, `--noflip` (GLPK dual-bound flip mode)

Gates:
- `make -C ralph test-simplex-policy`: pass
- `make -C ralph test-lu-markowitz`: pass
- `make -C ralph test-lp-policy-glpk-compat`: pass
- `make -C ralph test-netlib-gate-small`: pass (baseline-equivalent)
- `make -C ralph test-netlib-gate`: pass (baseline-equivalent)

Focused parity matrix:
- Full sweep executed: 72 combos = `3 cases × 3 methods × 2 steep × 2 relax × 2 flip`
- Results artifact: `/tmp/p3_full_matrix_v2.csv`

Observed outcomes:
- `pilot.ja.mps`: 0/24 optimal, mostly timeouts; dual path splits between timeout and error.
- `pilot.mps`: 0/24 optimal; primal path remains worst (timeouts/external timeouts), dual path mixes timeout and error.
- `stair.mps`: 8/24 optimal; all optimal runs required `flip=on` and `method in {dual,auto}`.
- Best `stair.mps` combo observed: `--method 1 --nosteep --relax --flip` (~1.26x GLPK wall time in that run).

Interpretation:
- Exposing GLPK-like `relax/flip` controls improved diagnosability and recovered optimal behavior on `stair` subsets.
- `pilot*` outliers are not resolved by control toggles alone; remaining gap is in solver robustness/per-iteration behavior on those families, not missing control-plane switches.

### P4 Scaffolding (Pilot-Family Focus Gate)

Added focused pilot-family diagnostics (non-gating):
- Allowlist: `ralph/benchmarks/netlib_pilot_focus.txt`
- Baseline: `ralph/benchmarks/netlib_pilot_focus_baseline.json`
- Make target: `make -C ralph test-netlib-gate-pilot-focus`

Current focused result (2026-03-01, method=primal):
- `pilot.ja.mps`, `pilot.mps`, `pilot.we.mps`, `pilot87.mps`: timeout (4/4)
- command failures: 0
- dense fallback files: 0

Interpretation:
- this confirms the dominant remaining issue in pilot-family is iteration/control robustness and/or per-iteration kernel efficiency under hard degenerate paths, not dense fallback routing.

### P4.1 Timeout22 Gate + BFCP Backend De-clamp (2026-03-05)

Deliverables:
- Added timeout22-focused NETLIB diagnostics gate:
  - allowlist: `ralph/benchmarks/netlib_timeout22.txt`
  - baseline: `ralph/benchmarks/netlib_timeout22_baseline.json`
  - target: `make -C ralph test-netlib-gate-timeout22`
- Removed BFCP backend clamping in runtime policy plumbing:
  - `lp_policy_glpk_compat_apply_runtime` now preserves requested
    `glpk_bfcp_backend` id.
  - `lp_bfcp_policy_compute` now accepts `luf_ft/cbg/cgr` as supported backend
    ids and preserves requested id in `effective_backend`.

Validation:
- `make -C ralph test-lp-policy-glpk-compat`: pass
- `make -C ralph test-lp-bfcp-policy`: pass
- `make -C ralph test-netlib-gate-timeout22`: pass
  - artifact: `/tmp/netlib-regression-gate-20260305-144634`
  - summary: 22/22 timeout files, 0 command failures, 0 status/objective/invalid mismatches, 0 dense fallback files

## GLPK-Compat Defaults (Planned)

For `lp_policy_profile=glpk_compat`:
- method: primal
- pricing: steep
- ratio: harris
- flip: off
- basis: adv
- presolve: on
- backend: luf_ft
- update limit / pivot tol / growth guard: auto (unless user overrides)

## Notes

- This roadmap supersedes/extends the older basis-governor-only framing.
- The older reference document (`docs/roadmaps/ralph_vs_glpk.md`) remains for
  historical context; this file is the active execution plan for GLPK-compat
  control-policy implementation.

## Next Execution Slice: G1 (BFCP Runtime Policy Module)

Objective:
- Move BFCP runtime normalization/clamping into a dedicated, orthogonal module so
  LU reinversion behavior is controlled by one global policy surface, not scattered
  local checks.

Scope (G1 only):
1. Add `lp_bfcp_policy` module:
   - `ralph/include/lp_bfcp_policy.h`
   - `ralph/src/lp_bfcp_policy.c`
2. Define a pure request -> effective mapping for:
   - backend support/normalization (`luf_ft/cbg/cgr` accepted backend ids)
   - update-limit override normalization
   - pivot/growth override normalization
3. Wire this module at solve setup in `ralph/src/ralph.c` before solver/LU override
   fields are assigned.
4. Add orthogonal unit coverage:
   - `ralph/tests/test_lp_bfcp_policy.c`

Constraints:
- No instance-specific behavior (no per-NETLIB switches, no filename branches).
- No algorithmic behavior change in G1; this is control-surface consolidation.
- Hard LU safety triggers remain unchanged and authoritative.

Promotion checks for G1:
- `make -C ralph test-lp-bfcp-policy`
- `make -C ralph test-lp-policy-glpk-compat`
- `make -C ralph test-lp-algorithm-api`

Follow-on (G2+):
- Consume `lp_bfcp_policy` decisions deeper in LU update/reinvert triggers
  (`lu_update`, `lu_needs_refactorization`) with reason-coded telemetry.

## G2 (In Progress): LU Trigger Integration + Reason Telemetry

Objective:
- Route LU reinvert decisions through BFCP refactor reasoning and expose
  trigger/failure reasons as first-class telemetry counters.

Scope:
1. Use `lp_bfcp_policy_refactor_reason(...)` in `lu_needs_refactorization(...)`.
2. Track `last_refactor_trigger_reason` on LU state and expose via LU telemetry snapshot.
3. Add LU telemetry counters for:
   - refactor-need checks/triggers by reason
   - LU update failure reasons (`max_updates`, `pivot_too_small`, etc.).
4. Surface the new LU reason counters in benchmark JSON for triage.

Constraints:
- Keep trigger ordering generic and policy-driven (no per-instance logic).
- Keep hard numerical safety triggers unchanged.
- Preserve baseline correctness expectations.

## G3 (Done): BFCP Lifecycle Helper Consolidation (No-Regression Slice)

Objective:
- Centralize BFCP lifecycle computations used by LU reinvert policy so update-limit
  and warmup semantics are policy-module controlled and independently testable.

Delivered:
1. Added policy helpers:
   - `lp_bfcp_policy_effective_update_limit(...)`
   - `lp_bfcp_policy_dense_reject_min_updates(...)`
2. Routed `lp_bfcp_policy_refactor_reason(...)` through the new helpers.
3. Extended unit coverage in `test_lp_bfcp_policy` for both helper APIs.
4. Kept default runtime behavior stable (no instance-specific tuning, no gate drift).

Validation:
- `make -C ralph test-lp-bfcp-policy`
- `make -C ralph test-lu-markowitz`
- `make -C ralph test-simplex-policy`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

## G4 (Done): BFCP Runtime Lifecycle Wiring (Adaptive Update Budget)

Objective:
- Wire BFCP lifecycle helpers into LU runtime so update-cap enforcement and
  reinvert reasoning consume one shared policy signal model.

Delivered:
1. `lu_update(...)` now uses BFCP `effective_update_limit` (cond-gated) for
   runtime max-update enforcement.
2. `lu_needs_refactorization(...)` and `lu_update(...)` now share one LU->BFCP
   signal builder (`lu_fill_bfcp_signals`) to avoid drift.
3. Added targeted tests:
   - BFCP helper behavior at/under `cond_min_updates`
   - LU runtime `COND_ADAPTIVE_LIMIT` enforcement path.

Validation:
- `make -C ralph test-lp-bfcp-policy`
- `make -C ralph test-lu-markowitz`
- `make -C ralph test-lp-telemetry-lu`
- `make -C ralph test-simplex-policy`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

Cleanup note:
- Removed simplex-local LU hard-trigger heuristic path and delegated it to BFCP/LU
  policy helpers (`lp_bfcp_policy_refactor_hard_trigger` + `lu_refactor_hard_trigger`),
  eliminating one legacy duplicated threshold block.
- Removed simplex-local basis action/refactor decision logic and delegated it to
  `lp_refactor_policy` (`lp_refactor_policy_choose_basis_action` and
  `lp_refactor_policy_phase1_small_pivot_refactor_allowed`) with dedicated policy tests.
- Removed simplex-local phase1 no-pivot force/ladder/cadence threshold logic and
  delegated it to `lp_refactor_policy`
  (`lp_refactor_policy_phase1_no_pivot_force_transition`,
  `lp_refactor_policy_phase1_no_pivot_ladder_step`,
  `lp_refactor_policy_phase1_dir_skip_ladder_rescue_due`), keeping telemetry in
  simplex and policy math in one module.
- Removed simplex-local phase1 force-pivot activation / dir-escape gate /
  force-relax / soft-LU-cooldown thresholds and delegated them to
  `lp_refactor_policy` (activation/escape/relax/cooldown helpers), leaving
  simplex as orchestration + telemetry only.
- Removed simplex-local phase1 reinvert-pressure and stagnation-decision
  heuristics and delegated them to `lp_refactor_policy`
  (`lp_refactor_policy_phase1_reinvert_pressure_safety_step`,
  `lp_refactor_policy_phase1_stagnation_escape_decision`), with dedicated
  policy tests and unchanged gate behavior.
- Removed simplex-local phase1 degen/stall/recompute/ratio-breakdown threshold
  constants and delegated those decisions to `lp_refactor_policy`
  (`lp_refactor_policy_phase1_degen_threshold`,
  `lp_refactor_policy_phase1_stall_threshold`,
  `lp_refactor_policy_phase1_recompute_interval`,
  `lp_refactor_policy_phase1_stall_obj_tol`,
  `lp_refactor_policy_phase1_ratio_breakdown_limit`), with dedicated
  unit tests in `test_lp_refactor_policy` and unchanged NETLIB gate behavior.
- Removed simplex-local periodic-feedback update heuristics and delegated the
  state transition to `lp_refactor_policy`
  (`lp_refactor_policy_periodic_feedback_set_hint`,
  `lp_refactor_policy_periodic_feedback_record_refactor`) via an orthogonal
  `LPPeriodicFeedbackState` API, with dedicated policy unit tests and unchanged
  NETLIB gate behavior.
- Removed simplex-local periodic pressure-decay arithmetic and delegated it to
  `lp_refactor_policy`
  (`lp_refactor_policy_periodic_pressure_effective`,
  `lp_refactor_policy_periodic_pressure_decay_recover`,
  `lp_refactor_policy_periodic_pressure_decay_penalty`) so cooldown pressure
  damping is policy-owned and unit-tested independently.
- Removed simplex-local periodic cooldown-state transitions and delegated them
  to `lp_refactor_policy`
  (`lp_refactor_policy_periodic_cooldown_tick`,
  `lp_refactor_policy_periodic_cooldown_extend`,
  `lp_refactor_policy_periodic_post_refactor_update`) so cooldown progression,
  soft-LU cooldown extension, and periodic post-refactor state updates are
  centralized in one policy module.
- Unified periodic scheduling decision plumbing behind one policy plan helper
  (`lp_refactor_policy_periodic_plan`) and routed both runtime simplex
  phase-1/phase-2 paths and `simplex_periodic_refactor_plan_for_test` through
  it, so cooldown eligibility, effective pressure, and run/suppress decisions
  are computed by one shared policy function.
- Replaced duplicated phase1/phase2 periodic-feedback state field mapping in
  `simplex.c` with tiny accessor/update helpers, so event handlers only pass
  `(phase, event)` and policy state transitions stay centralized.

## Remaining Gap to Verbatim GLPK Parity (2026-03-07)

The current roadmap closed much of the public control-surface gap, but not the
execution-path gap. Ralph now exposes many GLPK-shaped knobs, however strict
mode still does not execute one coherent GLPK-like policy lane end-to-end.

Observed remaining differences:
- strict mode is not authoritative across primal simplex, dual simplex, and LU
  at the same time
- primal phase-1 still carries Ralph-local rescue/stagnation machinery
- dual strict mode still runs on top of Ralph-local adaptive threshold logic
- LU/BFCP strict mode is still a policy wrapper around Ralph sparse-LU routing,
  not a backend-directed GLPK-like factor/update lane
- several GLPK semantics are still missing from the typed/runtime surface
  (`DUALP`, richer basis-init semantics, BTF, BFCP fine controls, `xcheck`,
  later `exact`)
- runtime parity is now blocked less by missing knobs and more by execution
  behavior and kernel cost

This means there are now two separate goals:
1. Functional parity: strict mode should make the same classes of control/LU
   decisions as GLPK.
2. Cost parity: once those decisions are aligned, Ralph's per-iteration kernels
   need to be cheap enough to approach GLPK wall time on NETLIB outliers.

## Gap-Closure Program

### A1: Authoritative Strict Control Plane

Objective:
- Make `glpk_strict_mode` the single authoritative control switch across
  primal simplex, dual simplex, and LU.

Scope:
1. Introduce a dedicated strict-policy executor module instead of scattering
   `if (strict)` branches across solver code.
2. Under strict mode, bypass Ralph-local control machinery that does not map to
   GLPK semantics:
   - phase-1 stagnation escape
   - phase-1 no-pivot ladder / rescue ladder
   - direction-stabilize force path
   - dual one-shot recovery
   - dual adaptive ratio thresholds from LU health/model size
   - LU spike-density / spike-work / cond-adaptive reinversion triggers
3. Keep only hard numerical safety stops authoritative in strict mode:
   - singular factor/update
   - invalid basis state
   - catastrophic instability

Implementation constraints:
- default profile behavior must remain unchanged
- no per-instance logic
- strict-mode branching must be unit-testable in isolation

Acceptance:
- strict mode decisions come from one module
- primal/dual/LU all respect the same strict profile semantics
- unit coverage proves the suppressed Ralph-local branches are inactive under
  strict mode

Suggested units:
- `ralph/tests/test_lp_glpk_strict_smcp.c`
- `ralph/tests/test_lp_glpk_strict_branching.c`

### A2: Complete SMCP Surface to GLPK Semantics

Objective:
- Close the remaining simplex-control API/behavior gap versus GLPK.

Scope:
1. Add missing method semantic:
   - `DUALP` (`dual first, then primal fallback`)
2. Add missing basis-init semantic surface:
   - `BIB`
   - `INI` / basis-load path
3. Preserve explicit SMCP controls as first-class policy data:
   - `method`
   - `pricing`
   - `ratio`
   - `flip`
   - `basis`
   - `presolve`
   - `tol_bnd`
   - `tol_dj`
   - `tol_piv`
   - `excl`
   - `shift`
   - `aorn`
4. Add exact final-basis verification control:
   - `xcheck`
5. Defer `exact` solve mode to the numerical-certification phase below.

Constraints:
- do not silently map missing GLPK semantics to legacy Ralph behavior
- if a strict-mode feature is not implemented yet, it should fail clearly

Acceptance:
- all SMCP-relevant semantics needed for LP parity have typed parameter IDs
- strict-mode runtime mapping is direct, not translated through ad hoc Ralph
  cadence heuristics

Suggested units:
- `ralph/tests/test_lp_glpk_strict_smcp.c`
- `ralph/tests/test_lp_basis_api.c`

### B1: Complete BFCP Surface to GLPK Semantics

Objective:
- Close the remaining basis-factorization control gap versus GLPK.

Scope:
1. Extend BFCP policy data with explicit GLPK-like fields:
   - factorization type (`LUF`, later `BTF+LUF`)
   - update engine (`FT`, `BG`, `GR`)
   - pivot tolerance
   - pivot search limit (`piv_lim`)
   - Suhl handling flag
   - epsilon tolerance
   - update limit (`nfs_max` style control)
   - Schur-update row/space controls (`nrs_max`-style control)
2. Stop deriving unrelated Ralph-local cadence knobs from BFCP settings in
   strict mode.
3. Keep BFCP normalization in one module and make all effective settings
   inspectable in telemetry/debug output.

Acceptance:
- strict-mode BFCP state fully describes the chosen factor/update behavior
- no strict-mode hidden translations from BFCP settings into unrelated simplex
  timers or rescue cadence

Suggested units:
- `ralph/tests/test_lp_glpk_strict_bfcp.c`

### B2: Add BTF Factorization Type

Objective:
- Match GLPK's exposed `BTF` factorization capability in the strict lane.

Scope:
1. Add typed/runtime support for BTF selection.
2. Add symbolic block-triangular decomposition path usable by the strict LU
   backend.
3. Use BTF only as an explicit backend/type choice in strict mode until the
   implementation is well characterized.

Acceptance:
- BTF is selectable and testable independently
- strict LU path can report whether BTF was used

Suggested units:
- `ralph/tests/test_lu_btf.c`

### C1: Build a Real Strict LU/BFCP Lane

Objective:
- Replace "GLPK-shaped policy over Ralph sparse-LU routing" with a true
  backend-directed strict LU execution path.

Scope:
1. Introduce a separate strict LU control/dispatch module.
2. In strict mode, route by BFCP backend/type directly rather than through the
   current Ralph orchestration of:
   - Markowitz retries
   - supernode cost gates
   - symbolic full-structural retry ladders
   - dense fallback heuristics
3. Preserve hard numerical failure exits, but remove Ralph-local adaptive
   routing logic from strict mode.
4. Keep the current sparse-LU orchestration as the default Ralph lane.

Constraints:
- do not delete the existing Ralph LU path during this phase
- strict LU must be independently selectable, inspectable, and testable

Acceptance:
- in strict mode, the chosen BFCP backend explains the numeric/update path
- refactor reasons map cleanly to BFCP semantics
- no Markowitz circuit breaker or supernode cost gate is active in strict mode

Suggested units:
- `ralph/tests/test_lu_glpk_strict.c`
- `ralph/tests/test_lu_backend_dispatch.c`

Current progress (2026-03-08):
- extracted backend-owned LU update operations into `ralph/src/lu_update_backend.c`
- separated BG/GR compatibility backends from eta-file state by giving them a
  dedicated sparse Schur-compat update lane
- preserved the existing FT kernel order verbatim after a first extraction
  changed `bore3d` behavior; this is now covered by the small NETLIB gate

### C2: Decouple Safety from Capacity Management

Objective:
- Ensure storage/capacity management supports the requested BFCP policy instead
  of silently rewriting effective strict-mode behavior.

Scope:
1. Separate allocation sizing from update-policy semantics.
2. Grow or provision storage to satisfy the active strict BFCP policy where
   feasible.
3. If capacity cannot satisfy the requested strict mode, fail explicitly rather
   than degrading into Ralph-local heuristics.

Acceptance:
- strict BFCP policy remains stable under varying workspace/storage pressure
- capacity exhaustion is telemetry-visible and explicit

Suggested units:
- `ralph/tests/test_lu_capacity_policy.c`

### D1: Add Numerical Certification (`xcheck`)

Objective:
- Provide GLPK-like final-basis verification to distinguish true infeasibility
  from numerical failure.

Scope:
1. Add final residual / primal-feasibility / dual-feasibility / complementary
   slackness certification on demand.
2. Expose explicit failure/status mapping for numerically doubtful end states.
3. Use this first as a strict-mode debug/verification tool and gate aid.

Acceptance:
- hard NETLIB outliers no longer end in ambiguous "infeasible vs unstable"
  states when `xcheck` is enabled
- certification can be tested independently of normal solve flow

Suggested units:
- `ralph/tests/test_lp_xcheck.c`

### D2: Add Exact Simplex Mode (Later)

Objective:
- Close the remaining numerical-diagnosis gap with GLPK's `exact` mode.

Scope:
1. Add a separate exact-simplex execution lane or exact-basis correction path.
2. Use this only after `xcheck` is in place and the strict floating-point lane
   is stable.

Acceptance:
- exact mode is explicit, orthogonal, and non-default
- correctness tests distinguish exact-vs-floating solve expectations

Note:
- this is important for functional parity and diagnosis, but not the first
  lever for runtime parity

### E1: Kernel-Cost Parity Program

Objective:
- Once strict control/LU behavior matches GLPK semantics, reduce Ralph's
  per-iteration cost to close the remaining wall-time gap.

Focus areas:
1. sparse FTRAN/BTRAN cost
2. update-factor path cost per event
3. reduced-cost recomputation frequency and cost
4. primal solution recomputation frequency and cost
5. steepest-edge / Devex weight maintenance quality and cost

Required telemetry:
- refactors
- updates
- ms/refactor
- ms/iter
- FTRAN/BTRAN nnz and runtime
- reduced-cost recompute counts and runtime
- solution recompute counts and runtime
- update rejection / reinversion trigger counts

Acceptance:
- improvements are generic and policy-agnostic
- outlier gains come from lower kernel cost, not instance-specific branching

## Verification Matrix for the Gap-Closure Program

Each phase above should be gated independently with:

Core unit gates:
- `make -C ralph test-lp-policy-glpk-compat`
- `make -C ralph test-lp-bfcp-policy`
- `make -C ralph test-simplex-policy`
- `make -C ralph test-lu-markowitz`

New strict-lane units:
- `make -C ralph test-lp-glpk-strict-smcp`
- `make -C ralph test-lp-glpk-strict-bfcp`
- `make -C ralph test-lu-glpk-strict`
- `make -C ralph test-lp-xcheck`

Focused GLPK parity set:
- `pilot.mps`
- `pilot.ja.mps`
- `pilot.we.mps`
- `pilot87.mps`
- `stair.mps`
- `degen3.mps`
- `fit1p.mps`
- `fit2p.mps`
- `wood1p.mps`
- `bore3d.mps`
- `capri.mps`

Full gates:
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

Success criteria for promotion:
1. no new correctness regressions
2. strict-mode telemetry is explainable from SMCP/BFCP settings
3. timeout reductions persist on repeated runs
4. improvements come from shared policy/kernel changes, not filename-specific
   tuning

## Recommended Execution Order

1. `A1` Authoritative strict control plane
2. `A2` Complete SMCP surface
3. `B1` Complete BFCP surface
4. `B2` Add BTF factorization type
5. `C1` Build strict LU/BFCP lane
6. `C2` Decouple safety from capacity management
7. `D1` Add `xcheck`
8. Re-run focused GLPK parity matrix
9. `D2` Add exact mode
10. `E1` Kernel-cost parity program

## Ranked Timeout-Reduction Plan

Current reference artifact:
- `/tmp/netlib-regression-gate-20260308-181712`

Current full-gate state:
- `84` files
- `22` timeout files
- `0` dense fallback files
- `0` status/objective/invalid mismatches

Interpretation:
- sparse-LU dense fallback is no longer the dominant blocker
- the remaining timeout set splits into kernel-cost, phase-1 recomputation, and
  degeneracy/control families
- timeout reduction should therefore be pursued by shared mechanism, not
  filename-specific tuning

### Week 1: Large-Basis Kernel-Cost Parity

Target family:
- `pilot.mps`
- `pilot.ja.mps`
- `pilot.we.mps`
- `pilot4.mps`
- `pilot87.mps`
- `pilotnov.mps`
- `fit2p.mps`
- `maros-r7.mps`
- `d2q06c.mps`

Observed pattern:
- Ralph often takes fewer iterations than GLPK on these files, but each
  iteration/refactor episode is much more expensive
- this is primarily a kernel-cost gap, not a pivot-count gap

Execution:
1. reduce large-basis refactor cost in the LU/update backend path
2. reduce sparse `FTRAN/BTRAN` cost per call and per nonzero
3. reduce update application cost for `FT` / `BG` / `GR`
4. add focused backend telemetry for:
   - `ms/refactor`
   - `ms/FTRAN`
   - `ms/BTRAN`
   - `ms/update`
   - nnz in/out per solve
5. compare `FT` vs `BG` vs `GR` on a focused gate before widening

Expected payoff:
- highest probability of retiring `6-9` timeout files per engineering week

Verification:
- focused gate: `pilot*`, `fit2p`, `maros-r7`, `d2q06c`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate`

Execution checklist (four atomic commits):
1. `W1.1` Telemetry split only
   - add refactor-stage and update-backend split telemetry through solver/LU
     snapshots and benchmark JSON
   - no behavior change
2. `W1.2` Large-basis refactor cost reduction
   - reduce avoidable work in refactor setup/finalize paths
   - preserve current control policy
3. `W1.3` Compact Schur backend caching
   - cache BG/GR compact-system factorizations and invalidate only when the
     update set changes
   - no control-policy change
4. `W1.4` Focused gates and backend invariants
   - add backend cache/invalidation tests
   - add a focused 9-file Week 1 gate
   - run small/full no-regression gates before promotion

Status:
- `W1.1` implemented
  - split `FTRAN/BTRAN` into base-vs-update-apply timing
  - split LU update telemetry into forward/backward apply and compact-solve
  - exposed the new counters in benchmark JSON
- `W1.2` first slice implemented
  - removed the temporary trailing `C_block` copy/copy-back from the supernode
    Schur update in `ralph/src/lu_supernode.c`
  - the update now writes directly into `A_struct` via scattered-row pointers
  - validation:
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-netlib-gate-small`
  - measured effect on `pilot.mps`:
    - `total_supernode_numeric_ms`: about `25989 ms -> 24490 ms`
    - total Ralph solve time: about `26592 ms -> 25710 ms`
  - result:
    - this is a real improvement in the pilot-family refactor kernel
    - it is not yet enough to retire the pilot-focus timeouts
    - next Week 1 work should continue on large-basis numeric refactor kernels,
      not revisit structural rebuild cost
- `W1.2` second slice implemented
  - reduced Markowitz pivot-row cleanup cost in `ralph/src/lu_sparse.c`
  - cleanup now reuses the existing row-to-column local-position hint
    (`rv_hint`) before falling back to a full column scan
  - validation:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-netlib-gate-small`
  - measured effect on `fit2p.mps`:
    - Ralph total solve time: about `29070 ms -> 28984 ms`
    - `total_markowitz_numeric_ms`: about `19408 ms -> 19041 ms`
  - result:
    - this is a small but real Markowitz-kernel improvement
    - `fit2p` remains dominated by Markowitz numeric time
- `W1.3` implemented
  - added cached compact forward/backward factorizations for the `BG` and `GR`
    update backends in `ralph/src/lu_update_backend.c`
  - added compact-factor telemetry and cache-invalidation tests in:
    - `ralph/src/lp_telemetry_lu.c`
    - `ralph/tests/test_lp_telemetry_lu.c`
    - `ralph/tests/test_lu_markowitz.c`
  - switched supernode workspace growth in `ralph/src/lu_sparse.c` to
    reusable `realloc`-backed storage instead of free/allocate churn
  - validation:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
  - result:
    - the compact backend lane is now structurally cheaper and explicitly
      testable
    - default `fit2p` remains Markowitz-dominated; the next hotspot work should
      instrument Markowitz exact scan costs instead of changing pivot semantics
- `W1.2` third slice implemented
  - replaced full affected-column `col_max` rescans in `ralph/src/lu_sparse.c`
    with exact incremental maintenance:
    - track current `col_max` position
    - mark a column dirty only when the current max can no longer be proven
    - rescan only dirty affected columns
  - this keeps Markowitz pivot semantics exact; it does not use stale maxima or
    relaxed eligibility thresholds
  - validation:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-netlib-gate-small`
    - `make -C ralph test-netlib-gate`
  - measured effect on `fit2p.mps`:
    - Ralph total solve time: about `33254 ms -> 24415 ms`
    - `mkz_col_max_scan_entries`: about `10.28B -> 47.44M`
    - `fit2p` moved from timeout-family behavior to solved in the current full
      gate baseline
  - result:
    - this is the first material Markowitz-kernel reduction in Week 1
    - the remaining `fit2p` cost is now in primary/update scan volume rather
      than `col_max` recomputation
- `W1.2` fourth slice implemented
  - added exact active-row degree histograms in `ralph/src/lu_sparse.c`
  - primary Markowitz scan now uses a provable lower bound:
    - if the minimum possible row degree cannot beat the current best
      Markowitz cost, the entire candidate column is skipped
    - in the non-reserved lane, tie-cost columns are also skipped when
      `col_max` cannot beat the current best pivot magnitude
  - this is exact pruning, not heuristic threshold tightening
  - validation:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-netlib-gate-small`
    - focused Week 1 allowlist gate
    - `make -C ralph test-netlib-gate`
  - measured effect:
    - direct `fit2p.mps`:
      - `mkz_primary_scan_entries`: about `53.56M -> 16.97M`
      - Ralph solve time class improved from about `24.4s` to about `17.9s`
    - focused Week 1 gate:
      - `fit2p.mps` solved in about `17.9s`
      - timeout-family count unchanged at `8/9`
  - result:
    - this is a real primary-scan reduction and a meaningful `fit2p`
      improvement
    - the remaining Week 1 large-basis gap is now more concentrated in
      update-existing/fill scan volume and large supernode numeric cost
- `W1.2` fifth slice implemented
  - cached live pivot-row structural column ids in `ralph/src/lu_sparse.c`
  - Markowitz update/fill/cleanup loops now reuse `pivot_live_col[]` directly
    instead of repeatedly reloading `rv_idx[pivot_live_rp[pe]]`
  - validation:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-netlib-gate-small`
    - focused Week 1 allowlist gate
    - `make -C ralph test-netlib-gate`
  - measured effect:
    - no timeout-family count change by itself
    - paired with the next supernode slice, `fit2p.mps` improved again in the
      full gate:
      - about `17.96s -> 17.47s`
  - result:
    - exact micro-kernel cleanup
    - small but baseline-safe reduction in Markowitz hot-loop indirection
- `W1.2` sixth slice implemented
  - added exact supernode Schur-update compaction in `ralph/src/lu_supernode.c`
  - the trailing GEMM now packs only:
    - trailing rows with nonzero supernode multipliers
    - trailing columns with nonzero supernode U entries
  - dense scattered-row update remains the fallback when the packed sets are
    full-size
  - validation:
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-netlib-gate-small`
    - focused Week 1 allowlist gate
    - `make -C ralph test-netlib-gate`
  - measured effect:
    - no new regressions
    - `pilot*` timeout-family count unchanged
    - direct `pilot.mps` supernode numeric stayed slightly lower than the
      older dense-trailing path, but not enough yet to retire pilot-family
      timeouts
  - result:
    - exact supernode numeric cost reduction is now structurally in place
    - the remaining pilot-family gap is still primarily supernode numeric cost,
      not Schur-update correctness or dense fallback
- `W1.2` seventh slice implemented
  - added exact supernode work telemetry:
    - active-row scan entries
    - active-col scan entries
    - trailing-vs-active row/column totals
    - L/U pack-entry totals
    - nominal dense-vs-compact triplet totals
    - compact/full/skipped update-call counts
  - validation:
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-netlib-gate-small`
  - measured effect on direct `pilot.mps`:
    - GLPK: about `2.25s`, `5232` iterations
    - Ralph: about `47.9s`, `430` iterations
    - `total_supernode_numeric_ms`: about `19.94s`
    - `sn_active_row_scan_entries`: about `2.03B`
    - `sn_active_col_scan_entries`: about `2.03B`
    - `sn_dense_triplets_total`: about `347.7B`
    - `sn_compact_triplets_total`: about `235.9M`
    - `sn_compact_update_calls`: about `290k`
    - `sn_skipped_update_calls`: about `1.92M`
  - result:
    - the dominant remaining pilot-family supernode cost is active-set discovery,
      not the compact update arithmetic itself
    - next Week 1 supernode work should target exact active-row/active-column
      discovery reuse, not another Schur-update arithmetic tweak
- `W1.2` eighth slice implemented
  - replaced the Step 3 supernode active-row/active-column nested rescans with
    exact activity marks that are populated during:
    - Step 1 multiplier generation, keyed by original row id
    - Step 2 trailing-column U emission, keyed by trailing-column offset
  - Step 3 still reconstructs `active_rows` and `active_cols` in the original
    ascending trailing order, so packed row/column construction semantics remain
    aligned with the old path
  - validation:
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
    - `make -C ralph test-netlib-gate`
  - measured effect:
    - direct `pilot.mps`:
      - Ralph total solve time: about `47.9s -> 25.4s`
      - `total_supernode_numeric_ms`: about `19.9s -> 8.3s`
      - `sn_active_row_scan_entries`: about `2.03B -> 1.72B`
      - `sn_active_col_scan_entries`: about `2.03B -> 1.71B`
    - isolated full NETLIB gate:
      - timeout count stayed at `21`
      - `fit2p.mps` remained solved and was back down to about `19.7s`
  - result:
    - this is the first supernode discovery reduction that stayed full-gate
      baseline-clean
    - pilot-family remains non-gating red, so the next Week 1 supernode work
      still needs to reduce event cost further, but this slice is safe to keep
- `W1.2` ninth slice implemented
  - removed per-supernode helper allocation/zero-fill churn from
    `ralph/src/lu_supernode.c`
  - the supernode numeric path now allocates:
    - `row_active_orig`
    - `col_active_local`
    - `touched_rows`
    - `touched_cols`
    - `active_rows`
    - `active_cols`
    once per factorization
  - each supernode clears only the touched row/column marks, while keeping the
    same final trailing-row/trailing-column reconstruction semantics
  - validation:
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
    - `make -C ralph test-netlib-gate`
  - measured effect:
    - direct `pilot.mps` stayed in the current improved band at about `25.2s`
    - isolated full NETLIB gate:
      - timeout count stayed at `21`
      - `fit2p.mps` stayed solved and improved to about `17.7s`
      - `0` dense fallback files
  - result:
    - this is a safe per-call cost reduction for the supernode compact path
    - it does not change numeric ordering or packed active-set semantics
- `W1.2` tenth slice implemented
  - added exact compact-update shape telemetry for supernode numeric work:
    - compact calls with active trailing columns `1`, `2`, `3`, `4`, and `5+`
    - total active rows seen in each bucket
  - exported through LU telemetry and benchmark JSON
  - validation:
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
  - measured effect:
    - direct `pilot.mps` shape sample:
      - `sn_compact_update_calls`: `230691`
      - `sn_compact_cols1_calls`: `63407`
      - `sn_compact_cols2_calls`: `42814`
      - `sn_compact_cols3_calls`: `18008`
      - `sn_compact_cols4_calls`: `24933`
      - `sn_compact_cols5p_calls`: `81529`
    - among exact compact-column shapes, `1` active trailing column is the
      dominant bucket on `pilot.mps`
  - result:
    - the next exact supernode specialization should target compact
      single-column updates first, not another broad compact-kernel rewrite
- `W1.2` eleventh slice implemented
  - added sampled supernode phase timing telemetry for:
    - panel factorization
    - right-of-supernode U emission
    - active-set discovery
    - compact block packing/build
    - full update kernel
    - compact update kernel
  - exported sample count as `sn_phase_samples` so the phase totals are
    interpreted as sampled diagnostic data, not full exact totals
  - important design correction:
    - the first exact always-on version was rejected because it inflated
      `sn_attempt_ms` enough to perturb the supernode cost gate and regressed
      `bore3d.mps`
    - the kept version uses deterministic sparse sampling of supernode
      factorization attempts to preserve baseline behavior
  - validation:
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
  - measured effect:
    - direct `pilot.mps` sampled phase mix:
      - `sn_phase_samples`: `29`
      - `sn_panel_factor_ms`: `112.287`
      - `sn_u_emit_ms`: `25.555`
      - `sn_active_set_ms`: `37.236`
      - `sn_pack_blocks_ms`: `0.528`
      - `sn_full_update_ms`: `0.000`
      - `sn_compact_update_ms`: `3.576`
    - compact kernel time is not the remaining pilot-family bottleneck
  - result:
    - the next supernode Week 1 target should be panel factorization and
      active-set discovery, not another compact-update specialization
- `W1.2` twelfth slice implemented
  - split sampled supernode panel timing into:
    - pivot search
    - row swap/scatter
    - in-panel elimination/update
  - kept this in the same sampled diagnostic lane as the phase timing slice,
    so it does not perturb the supernode cost gate
  - validation:
    - `make -C ralph test-lp-telemetry-lu`
    - `make -C ralph test-lu-supernode`
    - `make -C ralph test-netlib-gate-small`
  - measured effect:
    - direct `pilot.mps` sampled panel split:
      - `sn_phase_samples`: `33`
      - `sn_panel_factor_ms`: `77.390`
      - `sn_panel_pivot_search_ms`: `47.225`
      - `sn_panel_swap_scatter_ms`: `0.184`
      - `sn_panel_eliminate_ms`: `26.247`
      - `sn_u_emit_ms`: `16.889`
      - `sn_active_set_ms`: `28.542`
      - `total_supernode_numeric_ms`: `7974.686`
  - result:
    - sampled panel factor is not the dominant remaining pilot-family cost by
      itself
    - inside panel factor, pivot search is the only material subphase
    - row swap/scatter is negligible, so it should not be a Week 1 target
- rejected during `W1.2`
  - stale or approximate `col_max` shortcuts and other behavior-adjacent
    Markowitz optimizations were tried and rolled back
  - the accepted Week 1 `col_max` change is exact maintenance, not caching with
    stale maxima

### Week 2: Phase-1 Recompute Suppression

Target family:
- `greenbeb.mps`
- `wood1p.mps`
- `woodw.mps`
- `perold.mps`
- `stocfor2.mps`
- `cycle.mps`

Observed pattern:
- phase 1 dominates runtime
- repeated no-progress, ratio-breakdown, and full-state recomputation consume
  most of the budget

Execution:
1. separate "full solution recompute" from "reduced-cost refresh"
2. preserve more incremental phase-1 state across no-progress / dir-skip loops
3. tighten recompute triggers so instability handling does not force full
   vector rebuilds unnecessarily
4. add telemetry for recompute reason, full-refresh reason, and rc-only refresh

Expected payoff:
- likely retirement of `4-6` timeout files

Verification:
- focused gate: `greenbeb`, `wood1p`, `woodw`, `perold`, `stocfor2`, `cycle`
- no regression on small/full NETLIB

Completed slices:
1. `W2.1` bound phase-1 dual-rescue churn.
   - committed as `b94fe5b`
   - added compute-context attribution for dual rescue
   - reduced rescue-dominated recompute cost on `greenbeb` and `wood1p`
     without changing the global timeout count
2. `W2.2` widen phase-1 `dir_skip` rc-only / no-recompute eligibility by LP
   width, not just basis height.
   - small-basis but wide LPs now enter the existing `dir_skip`
     recompute-suppression lane when `n >= 2000`
   - focused `wood1p`/`greenbeb` gate stayed baseline-clean
   - full NETLIB gate remained baseline-clean at `22` timeouts with `0`
     dense fallbacks and no new mismatches
   - direct `wood1p` now uses the intended mechanism:
     `dir_stabilize_skip_no_recompute = 77` and
     `recompute_after_dir_skip = 3469 -> 1870`

3. `W2.3` arm existing force-pivot mode from chronic extreme-direction
   refactor loops without queueing an extra forced refactor.
   - committed as `5d33ff4`
   - the new path counts repeated `force_extreme_dir` refactors into the
     existing force-pivot budget instead of scheduling another immediate
     forced refactor
   - direct `wood1p` improved materially:
     `dir_stabilize_refactor_from_force_extreme_dir = 1863 -> 416`,
     `dir_stabilize_refactor_from_force_pivot_mode = 0 -> 34`,
     `refactor_calls = 1952 -> 707`,
     `recompute_after_dir_skip = 1870 -> 448`,
     `time_ms = 3262.617 -> 2541.829`
   - direct `greenbeb` also improved in wall time
     (`7416.830 -> 6610.409 ms`) but remained timeout-bound
   - focused `wood1p`/`greenbeb` gate and full NETLIB gate both stayed
     baseline-clean:
     `22` timeouts, `0` dense fallbacks, and no new mismatches
4. `W2.4` force ratio-breakdown ladder escalation earlier on large degenerate
   runs, and add direct dual-rescue guard telemetry.
   - committed as `c35c31f`
   - added direct phase-1 dual-rescue guard/attempt telemetry to separate
     direct rescue sites from ladder rescue sites
   - finding: current `greenbeb`/`wood1p` runs are still dominated by ladder
     rescue / retry pressure, not direct rescue
   - tightened the large-degenerate ratio-breakdown ladder so repeated
     no-pivot ratio failures force recovery earlier without waiting for an
     extra no-progress streak
   - full NETLIB gate stayed baseline-clean at `22` timeouts, `0` dense
     fallbacks, and no new mismatches
   - `greenbeb` improved materially in the full gate:
     `time_ms = 9224.610 -> 6528.542`,
     `iterations = 5052 -> 1624`,
     `ratio_breakdown_retries = 2785 -> 540`,
     `no_pivot_ladder_forced_refactors_ratio_breakdown = 20 -> 110`
   - `wood1p` stayed in the same timeout family but did not regress on the
     full gate (`3267.603 -> 3255.448 ms`)
5. `W2.5` penalize repeated failed-stabilize enterings only inside the local
   direction-stabilize retry path.
   - repeated failed-stabilize telemetry showed `wood1p`/`greenbeb` were
     dominated by retrying the same entering after direction-stabilize, not by
     plain top-level `DIR_SKIP` reselection
   - the new path does not change top-level phase-1 pricing or global entering
     exclusion memory
   - it only swaps the local post-refactor retry from the repeated unstable
     entering to one alternate candidate after the failed-stabilize streak
     reaches the trigger
   - added pure-policy unit coverage in `ralph/tests/test_simplex_policy.c`
   - validation:
     - `make -C ralph test-simplex-policy`
     - direct `wood1p` and `greenbeb`
     - `make -C ralph test-netlib-gate`
   - direct effects:
     - `wood1p`: `3130.986 -> 2358.580 ms`
     - `greenbeb`: `8062.355 -> 6598.889 ms`
   - full NETLIB gate stayed baseline-clean:
     - `84` files
     - `22` timeouts
     - `0` command failures
     - `0` status/objective/invalid mismatches
     - `0` dense fallbacks
6. `W2.6` instrument the local failed-stabilize retry lane before adding more
   retry memory.
   - added explicit telemetry for the local retry path:
     - penalty arms
     - alternate found vs no-alt
     - alternate stabilized vs failed
     - same-alternate repeat streaks
   - validation:
     - `make -C ralph test-lp-telemetry-solver`
     - `make -C ralph test-simplex-policy`
     - `make -C ralph test-netlib-gate-small`
     - `make -C ralph test-netlib-gate`
   - direct findings on the two target outliers:
     - `wood1p`: all local retry-penalty counters stayed `0`
     - `greenbeb`: all local retry-penalty counters stayed `0`
   - implication:
     - the current localized failed-stabilize retry penalty is not active on
       the Week 2 outliers
     - adding a bounded two-slot local retry memory now would be dead code for
       those files
     - the next Week 2 change should target why the localized retry lane is not
       reached, rather than widening its memory
7. `W2.7` retarget the localized failed-stabilize retry penalty to the actual
   failed alternate and bound it with a higher trigger.
   - added explicit primary-vs-alternate failed-stabilize site telemetry so the
     retry trace distinguishes whether the final unstable candidate was the
     original post-refactor entering or the local alternate
   - the local retry penalty now keys off the last failed retry candidate, not
     the pre-retry original entering
   - kept the policy narrow by raising the arm threshold from `2` to `3`
     repeated alternate failures; the `2`-streak version activated correctly
     but over-fired and drove `wood1p`/`greenbeb` into much worse timeout
     behavior
   - validation:
     - `make -C ralph test-lp-telemetry-solver`
     - `make -C ralph test-simplex-policy`
     - direct `wood1p` / `greenbeb`
     - `make -C ralph test-netlib-gate-small`
     - `make -C ralph test-netlib-gate`
   - direct findings with the kept `3`-streak trigger:
     - `wood1p`: retry lane active, `failed_stabilize_alternate_failures=569`,
       `failed_stabilize_retry_penalty_arms=140`, `time_ms=2402.204`,
       `iterations=927`
     - `greenbeb`: retry lane active, `failed_stabilize_alternate_failures=246`,
       `failed_stabilize_retry_penalty_arms=61`, `time_ms=6535.083`,
       `iterations=1622`
   - full NETLIB gate stayed baseline-clean:
     - `84` files
     - `22` timeouts
     - `0` command failures
     - `0` status/objective/invalid mismatches
     - `0` dense fallbacks
   - implication:
     - the localized retry lane is now hitting the real failed alternate path
     - the remaining Week 2 gap is not trigger reachability anymore; it is the
       quality of the alternate selected once the lane is active
8. `W2.8` add repeat-aware local alternate memory inside the active retry lane.
   - added explicit retry-lane telemetry for the local-memory mode:
     - arms
     - alternate found
     - no alternate
     - fallback to the same prior alternate
     - alternate stabilized vs failed
   - the improvement stays local to direction-stabilize only:
     - top-level phase-1 pricing is unchanged
     - global entering-exclusion memory is unchanged
     - the new path only avoids reusing the same retry alternate after that
       alternate has already repeated (`trigger=2`)
   - direct effects:
     - `wood1p`: `2402.204 -> 2335.354 ms`, `927 -> 919` iterations
       with `failed_stabilize_retry_local_memory_arms=185`
     - `greenbeb`: `6535.083 -> 6433.517 ms`, `1622 -> 1621` iterations
       with `failed_stabilize_retry_local_memory_arms=81`
   - validation:
     - `make -C ralph test-lp-telemetry-solver`
     - `make -C ralph test-simplex-policy`
     - direct `wood1p` / `greenbeb`
     - `make -C ralph test-netlib-gate-small`
     - `make -C ralph test-netlib-gate`
   - full NETLIB gate stayed baseline-clean:
     - `84` files
     - `22` timeouts
     - `0` command failures
     - `0` status/objective/invalid mismatches
     - `0` dense fallbacks
   - implication:
     - the retry lane now distinguishes:
       - penalty on the last failed retry candidate
       - local-memory avoidance of a repeated retry alternate
     - the next Week 2 question is whether alternate quality can be improved
       further without widening the retry lane beyond these local guards
9. `W2.9` instrument the active retry pool before widening alternate selection.
   - first, rejected a bounded stronger-selector experiment:
     - direct `wood1p` / `greenbeb` showed
       `failed_stabilize_retry_stronger_selector_nonbland_picks=0`
     - implication: a stronger selector over only the first few alternates
       just added scan overhead; bland was already picking the same candidate in
       that bounded local window
   - then added sampled retry-pool telemetry for the actual active retry lane:
     - samples
     - eligible-total / eligible-max
     - singleton-sample count
     - best-differs-from-bland sample count
   - validation:
     - `make -C ralph test-lp-telemetry-solver`
     - `make -C ralph test-simplex-policy`
     - `make -C ralph build-ralph-benchmark`
     - `make -C ralph test-netlib-gate-small`
     - `make -C ralph test-netlib-gate`
   - direct findings on the target outliers:
     - `wood1p`:
       - `failed_stabilize_retry_pool_samples=27`
       - `failed_stabilize_retry_pool_eligible_total=29181`
       - `failed_stabilize_retry_pool_eligible_max=1281`
       - `failed_stabilize_retry_pool_singleton_samples=0`
       - `failed_stabilize_retry_pool_best_differs_samples=27`
       - direct time stayed in the same band: `2709.092 ms`, `773` iterations
     - `greenbeb`:
       - `failed_stabilize_retry_pool_samples=2`
       - `failed_stabilize_retry_pool_eligible_total=2970`
       - `failed_stabilize_retry_pool_eligible_max=1620`
       - `failed_stabilize_retry_pool_singleton_samples=0`
       - `failed_stabilize_retry_pool_best_differs_samples=2`
       - direct time stayed in the same band: `7503.407 ms`, `1391` iterations
10. `W2.10` add retry-lane selector-class telemetry and evaluate a guarded broader
    alternate selector.
    - added exact retry-lane selector telemetry:
      - bland arms
      - guarded arms
      - guarded eligible-total / eligible-max
      - bland alternate stabilized / failed
      - guarded alternate stabilized / failed
    - added a tightly gated broader selector only inside the local retry lane:
      - no top-level phase-1 pricing change
      - no global entering-exclusion change
      - broader pool scan only when the retry alternate streak is already
        chronic (`>= 4`)
      - guarded pick only if the broader best candidate differs from bland and
        clears a large score-ratio gate (`>= 16x`)
    - validation:
      - `make -C ralph test-lp-telemetry-solver`
      - `make -C ralph test-simplex-policy`
      - `make -C ralph build-ralph-benchmark`
      - direct `wood1p` / `greenbeb`
      - `make -C ralph test-netlib-gate-small`
      - `make -C ralph test-netlib-gate`
    - direct findings on the target outliers:
      - `wood1p`:
        - `failed_stabilize_retry_selector_bland_arms=118`
        - `failed_stabilize_retry_selector_guarded_arms=0`
        - direct time: `2871.742 ms`, `719` iterations
      - `greenbeb`:
        - `failed_stabilize_retry_selector_bland_arms=21`
        - `failed_stabilize_retry_selector_guarded_arms=0`
        - direct time: `8012.630 ms`, `1435` iterations
    - full NETLIB gate stayed baseline-clean:
      - `84` files
      - `22` timeouts
      - `0` command failures
      - `0` status/objective/invalid mismatches
      - `0` dense fallbacks
    - implication:
      - the broader selector is currently too conservative to activate on the
        real Week 2 outliers
      - keeping the selector-class telemetry is useful
      - the next Week 2 move should not be a broader blind selector; it needs a
        better safety predicate or a different post-selection diagnostic
11. `W2.11` classify retry-lane selector failures by cause and gate broader
    selection on repeated ratio-driven failures.
    - added exact post-selection failure telemetry by selector class:
      - bland ratio-failed / dir-failed
      - guarded ratio-failed / dir-failed
      - guarded fallback-to-bland count
    - added a safer guarded-selector lane:
      - broader selector remains local to direction-stabilize retry
      - if a guarded pick ever fails immediately, it can fall back to the bland
        retry alternate inside the same retry episode
      - broader selection is now only eligible after repeated ratio-driven retry
        failures, not just repeated alternate reuse
    - validation:
      - `make -C ralph test-lp-telemetry-solver`
      - `make -C ralph test-simplex-policy`
      - `make -C ralph build-ralph-benchmark`
      - direct `wood1p` / `greenbeb`
      - `make -C ralph test-netlib-gate-small`
      - `make -C ralph test-netlib-gate`
    - direct findings on the target outliers:
      - `wood1p`:
        - `failed_stabilize_retry_selector_guarded_arms=0`
        - `failed_stabilize_retry_selector_bland_ratio_failed=0`
        - `failed_stabilize_retry_selector_bland_dir_failed=187`
        - `time_ms=2379.254`, `iterations=927`
      - `greenbeb`:
        - `failed_stabilize_retry_selector_guarded_arms=0`
        - `failed_stabilize_retry_selector_bland_ratio_failed=0`
        - `failed_stabilize_retry_selector_bland_dir_failed=84`
        - `time_ms=6566.517`, `iterations=1631`
    - full NETLIB gate stayed baseline-clean:
      - `84` files
      - `22` timeouts
      - `0` command failures
      - `0` status/objective/invalid mismatches
      - `0` dense fallbacks
    - implication:
      - the active retry-lane failures on `wood1p` / `greenbeb` are direction
        failures, not ratio failures
      - the broader selector is not the next real lever
      - the next Week 2 move should instrument why these alternates keep
        producing unstable directions after refactor, rather than widening
        alternate scoring further
   - full NETLIB gate stayed baseline-clean:
     - `84` files
     - `22` timeouts
     - `0` command failures
     - `0` status/objective/invalid mismatches
     - `0` dense fallbacks
   - implication:
     - the retry pool is not small or singleton-limited on these outliers
     - the remaining Week 2 gap is broader alternate-pool construction /
       selection inside the active retry lane, not post-selection ratio work
     - the next Week 2 change should widen local alternate selection in the
       retry lane itself, while keeping top-level phase-1 pricing unchanged

12. `W2.12` instrument retry-lane unstable-direction shape and gate original-entering
    exclusion on catastrophic retry directions.
    - added exact retry-lane direction-shape telemetry:
      - `failed_stabilize_retry_dir_fail_shape_samples`
      - `failed_stabilize_retry_dir_fail_nnz_total/max`
      - `failed_stabilize_retry_dir_fail_dir_inf_total/max`
      - `failed_stabilize_retry_dir_fail_pivot_abs_total/max`
    - direct telemetry on the baseline telemetry-only slice showed:
      - `wood1p` retry-lane direction failures are much more extreme than
        `greenbeb`
      - `wood1p`: `190` samples, `dir_inf_max=8.51e8`,
        `pivot_abs_max=253.49`
      - `greenbeb`: `83` samples, `dir_inf_max=2.62e5`,
        `pivot_abs_max=0.756`
    - tried a bounded retry-lane second-chance alternate:
      - rejected
      - it stayed correctness-clean but was not a good baseline:
        - `wood1p`: `89` arms, `2` stabilized, `87` failed
        - it re-inflated other hotspots, especially `fit2p`
    - kept the telemetry and added a narrow direction-quality guard instead:
      - when local retry memory is active and the retry direction is
        catastrophically unstable, exclude the original entering for a short
        bounded window using the existing exclusion memory
      - predicate:
        - repeated retry episode
        - `dir_inf >= 10x` the direction-stabilize threshold
        - `dir_nnz >= 64`
        - `pivot_abs / dir_inf <= 1e-5`
      - original-entering exclusion window: `4` iterations
    - direct result with the kept guard:
      - `wood1p`:
        - `failed_stabilize_retry_dir_guard_arms=183`
        - `time_ms=2375.085`, `iterations=925`
      - `greenbeb`:
        - `failed_stabilize_retry_dir_guard_arms=79`
        - `time_ms=6456.893`, `iterations=1615`
    - gate result:
      - small NETLIB gate passed
      - full NETLIB gate stayed baseline-clean:
        - `84` files
        - `22` timeouts
        - `0` command failures
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - the retry-lane direction path is now observable and lightly governed
      - the guard is not a breakthrough, but it is a safe directional baseline
      - the next Week 2 lever should be a better direction-quality discriminator,
        not broader alternate scoring

13. `W2.13` bucket retry-lane direction failures by direction scale and
    pivot-to-direction ratio.
    - added exact retry-direction telemetry buckets:
      - `failed_stabilize_retry_dir_fail_inf_ratio_le_30`
      - `failed_stabilize_retry_dir_fail_inf_ratio_le_100`
      - `failed_stabilize_retry_dir_fail_inf_ratio_le_1000`
      - `failed_stabilize_retry_dir_fail_inf_ratio_gt_1000`
      - `failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8`
      - `failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6`
      - `failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4`
      - `failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4`
    - direct bucketed classification showed a real split between the two Week 2
      outlier families:
      - `wood1p`:
        - `185` retry-direction failure samples
        - `137` in `dir_inf / threshold <= 30`
        - `21` in `<= 100`
        - `27` in `> 1000`
        - pivot ratio buckets:
          - `155` in `pivot_abs / dir_inf <= 1e-8`
          - `27` in `<= 1e-6`
          - `3` in `<= 1e-4`
      - `greenbeb`:
        - `79` retry-direction failure samples
        - all `79` in `dir_inf / threshold <= 30`
        - all `79` in `1e-6 < pivot_abs / dir_inf <= 1e-4`
    - implication:
      - `greenbeb` is severe but not catastrophic on retry-direction shape
      - `wood1p` has a distinct catastrophic weak-pivot tail that `greenbeb`
        does not
      - the next discriminator should use these bucket families rather than
        another blind scalar threshold guess
    - gate result:
      - small NETLIB gate passed:
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - `84` files
        - `22` timeouts
        - `0` command failures
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks

14. `W2.14` retarget the retry-direction guard to the catastrophic weak-pivot
    bucket only.
    - changed the retry-direction guard from a broad scalar pivot-ratio gate
      (`pivot_abs / dir_inf <= 1e-5`) to the bucket boundary that actually
      separates the Week 2 outliers:
      - `pivot_abs / dir_inf <= 1e-8`
    - kept the existing repeat-count, direction-scale, and direction-nnz
      preconditions unchanged
    - added a policy test to ensure the guard stays off on the
      `greenbeb`-class severe-but-non-catastrophic case
    - direct result:
      - `wood1p`:
        - `failed_stabilize_retry_dir_guard_arms=154`
        - `time_ms=2364.274`
        - `iterations=920`
      - `greenbeb`:
        - `failed_stabilize_retry_dir_guard_arms=0`
        - `time_ms=6531.290`
        - `iterations=1633`
    - gate result:
      - small NETLIB gate passed:
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - `84` files
        - `22` timeouts
        - `0` command failures
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - the retry-direction guard is now aligned to the observed catastrophic
        `wood1p` tail instead of the milder `greenbeb` failure family
      - next Week 2 work should preserve this split and focus on the remaining
        severe-but-non-catastrophic retry directions separately

15. `W2.15` measure retry-lane selector score gaps on the active local-memory
    path instead of only on the dormant guarded-selector branch.
    - added exact selector-eval telemetry for the active retry lane:
      - eval samples
      - best-differs samples
      - score-ratio total / max
      - score-ratio `>=2` / `>=4`
    - direct result:
      - `greenbeb`:
        - `failed_stabilize_retry_selector_eval_samples=82`
        - `failed_stabilize_retry_selector_eval_best_differs_samples=82`
      - `wood1p`:
        - `failed_stabilize_retry_selector_eval_samples=186`
        - `failed_stabilize_retry_selector_eval_best_differs_samples=186`
    - implication:
      - materially better devex-scored retry alternates do exist on both
        Week 2 outliers
      - the gap is not “no better candidate exists”; it is using that
        information safely

16. `W2.16` add a shadow-only retry-lane broad-alternate comparison that
    records the best-scored alternate's post-selection direction proxy without
    changing any pivot decisions.
    - added shadow retry telemetry:
      - shadow samples
      - ratio-failed samples
      - direction-stable vs direction-failed samples
      - shadow direction nnz / inf-norm / pivot-abs totals and maxima
    - direct result:
      - `greenbeb`:
        - `failed_stabilize_retry_shadow_samples=75`
        - `failed_stabilize_retry_shadow_ratio_failed=0`
        - `failed_stabilize_retry_shadow_dir_stable=0`
        - `failed_stabilize_retry_shadow_dir_failed=75`
      - `wood1p`:
        - `failed_stabilize_retry_shadow_samples=205`
        - `failed_stabilize_retry_shadow_ratio_failed=0`
        - `failed_stabilize_retry_shadow_dir_stable=0`
        - `failed_stabilize_retry_shadow_dir_failed=205`
    - gate result:
      - small NETLIB gate passed:
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - `84` files
        - `22` timeouts
        - `0` command failures
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - broader retry-lane alternate scoring is not the next Week 2 lever
      - even the best-scored alternate in the retry pool still yields an
        unstable post-refactor direction on these outliers

17. `W2.17` guard severe retry-lane directions when the shadow-best alternate
    is provably even more toxic.
    - added a retry-lane shadow-toxic guard:
      - only after repeated local retry episodes
      - only for severe-but-non-catastrophic actual retry directions
      - only when the shadow-best alternate passes the ratio test but projects
        a much larger direction and a much weaker pivot ratio
      - action stays local and conservative: briefly exclude the original
        entering through the existing exclusion memory
    - added explicit telemetry:
      - shadow guard arms
      - shadow guard original-entering exclusions
    - direct result:
      - `wood1p`:
        - `time_ms=2285.335`
        - `iterations=906`
        - `failed_stabilize_retry_shadow_guard_arms=27`
      - `greenbeb`:
        - `time_ms=6217.478`
        - `iterations=1613`
        - `failed_stabilize_retry_shadow_guard_arms=78`
    - gate result:
      - small NETLIB gate passed:
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - `84` files
        - `22` timeouts
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - the retry-lane pathology is now split three ways:
        - catastrophic weak-pivot tail (`wood1p`-class)
        - severe-but-non-catastrophic shadow-toxic tail (`greenbeb`-class)
        - everything else
      - this is a safe Week 2 baseline, but it is still a modest behavior win,
        not the timeout-count breakthrough
      - the remaining Week 2 problem is the direction pathology itself, not
        selector breadth

18. `W2.18` classify the post-shadow-toxic follow-up path.
    - added telemetry for two separate views of a shadow-toxic episode:
      - immediate post-shadow `DIR_SKIP` ladder outcome:
        - plain retry
        - dual-rescue attempt
        - forced-refactor path
      - next later event after the shadow-toxic episode:
        - another failed-stabilize
        - ratio breakdown
        - pivot failure
        - pivot success
    - focused result:
      - `wood1p`:
        - `failed_stabilize_retry_shadow_guard_arms=27`
        - `failed_stabilize_retry_shadow_post_dir_skip_retry=27`
        - `failed_stabilize_retry_shadow_post_dir_skip_dual_rescue=0`
        - `failed_stabilize_retry_shadow_post_dir_skip_forced_refactor=0`
        - `failed_stabilize_retry_shadow_next_failed_stabilize=27`
        - `failed_stabilize_retry_shadow_next_ratio_breakdown=0`
        - `failed_stabilize_retry_shadow_next_pivot_fail=0`
        - `failed_stabilize_retry_shadow_next_pivot_success=0`
        - `time_ms=2391.901`
        - `iterations=926`
      - `greenbeb`:
        - `failed_stabilize_retry_shadow_guard_arms=72`
        - `failed_stabilize_retry_shadow_post_dir_skip_retry=70`
        - `failed_stabilize_retry_shadow_post_dir_skip_dual_rescue=0`
        - `failed_stabilize_retry_shadow_post_dir_skip_forced_refactor=2`
        - `failed_stabilize_retry_shadow_next_failed_stabilize=72`
        - `failed_stabilize_retry_shadow_next_ratio_breakdown=0`
        - `failed_stabilize_retry_shadow_next_pivot_fail=0`
        - `failed_stabilize_retry_shadow_next_pivot_success=0`
        - `time_ms=6287.647`
        - `iterations=1594`
    - gate result:
      - small NETLIB gate passed:
        - artifact: `/tmp/netlib-regression-gate-20260313-095342`
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - artifact: `/tmp/netlib-regression-gate-20260313-095348`
        - `84` files
        - `22` timeouts
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - the dominant `greenbeb`/`wood1p` loop is now explicit:
        - shadow-toxic exclusion
        - immediate plain `DIR_SKIP` retry
        - return to another failed-stabilize event
      - the remaining Week 2 lever is retry-direction quality after the
        `DIR_SKIP` recompute, not exclusion length, ratio-breakdown handling,
        pivot-failure handling, or broader selector scoring

19. `W2.19` classify the post-shadow follow-up direction after recompute.
    - added telemetry for the first post-shadow follow-up direction:
      - sample count
      - direction nnz total/max
      - direction inf-norm total/max
      - leaving pivot abs total/max
      - theta total/max
      - follow-up class:
        - bound geometry
        - bound flip
        - tiny theta
        - weak leaving
        - FTRAN-shape remainder
    - focused result:
      - `wood1p`:
        - `failed_stabilize_retry_shadow_followup_dir_samples=27`
        - `failed_stabilize_retry_shadow_followup_dir_bound_geometry=27`
        - `failed_stabilize_retry_shadow_followup_dir_bound_flip=27`
        - `failed_stabilize_retry_shadow_followup_dir_tiny_theta=0`
        - `failed_stabilize_retry_shadow_followup_dir_weak_leaving=0`
        - `failed_stabilize_retry_shadow_followup_dir_ftran_shape=0`
        - `failed_stabilize_retry_shadow_followup_dir_nnz_max=117`
        - `failed_stabilize_retry_shadow_followup_dir_inf_max=28474280.162531`
        - `failed_stabilize_retry_shadow_followup_theta_max=5.623474`
        - `time_ms=2389.020`
        - `iterations=925`
        - artifact: `/tmp/wood1p_shadow_followup_dirclass.json`
      - `greenbeb`:
        - `failed_stabilize_retry_shadow_followup_dir_samples=83`
        - `failed_stabilize_retry_shadow_followup_dir_bound_geometry=83`
        - `failed_stabilize_retry_shadow_followup_dir_bound_flip=0`
        - `failed_stabilize_retry_shadow_followup_dir_tiny_theta=83`
        - `failed_stabilize_retry_shadow_followup_dir_weak_leaving=0`
        - `failed_stabilize_retry_shadow_followup_dir_ftran_shape=0`
        - `failed_stabilize_retry_shadow_followup_dir_nnz_max=233`
        - `failed_stabilize_retry_shadow_followup_dir_inf_max=10291472.664632`
        - `failed_stabilize_retry_shadow_followup_theta_max=0.000000`
        - `time_ms=6468.134`
        - `iterations=1628`
        - artifact: `/tmp/greenbeb_shadow_followup_dirclass.json`
    - gate result:
      - small NETLIB gate passed:
        - artifact: `/tmp/netlib-regression-gate-20260313-103210`
        - `27/27`
        - `0` timeouts
        - `0` dense fallbacks
      - full NETLIB gate stayed baseline-clean:
        - artifact: `/tmp/netlib-regression-gate-20260313-103228`
        - `84` files
        - `22` timeouts
        - `0` status/objective/invalid mismatches
        - `0` dense fallbacks
    - implication:
      - the post-shadow follow-up path now separates cleanly:
        - `wood1p` is a bound-flip follow-up problem
        - `greenbeb` is a tiny-theta follow-up problem
      - neither file is primarily a weak-leaving or generic FTRAN-shape case
      - the next Week 2 lever should split these two follow-up geometries
        instead of widening retry-lane scoring again

20. Rejected after `W2.18`: one-shot post-shadow non-Bland pricing override.
    - tried a bounded immediate follow-up that bypassed one forced Bland
      pricing step after a shadow-toxic exclusion
    - result:
      - `wood1p` worsened materially:
        - about `2391.901 ms`, `926` iterations -> `2360.996 ms`, `3535` iterations
      - `greenbeb` did not improve enough to justify the change:
        - about `6287.647 ms`, `1594` iterations -> `6504.324 ms`, `1615` iterations
    - conclusion:
      - immediate post-shadow pricing mode is not the right Week 2 lever
      - the remaining issue is still retry-direction quality, not lack of a
        stronger entering score on the next priced iteration

### Week 3: Degeneracy and Long-Run Control Quality

Target family:
- `d6cube.mps`
- `degen3.mps`
- `greenbea.mps`
- `bnl1.mps`
- `bnl2.mps`
- `maros.mps`
- `fffff800.mps`

Observed pattern:
- Ralph takes many more pivots than GLPK on these files
- this is a long-run degeneracy/control gap more than a per-iteration cost gap

Execution:
1. improve pricing-weight maintenance quality on long degenerate runs
2. revisit reinversion cadence using pivot-quality / solve-sparsity telemetry,
   not static intervals alone
3. reduce long-run control drift without introducing instance-specific logic
4. compare pivot counts directly against GLPK on the focused set

Expected payoff:
- likely retirement of `3-5` timeout files
- `d6cube` remains the hardest case and may need multiple slices

Verification:
- focused gate: `d6cube`, `degen3`, `greenbea`, `bnl1`, `bnl2`, `maros`,
  `fffff800`
- require no regression on the Week 1 and Week 2 families

Progress:
- `W3.1` phase-2 degeneracy telemetry baseline is now in the solver.
- Added measurement-only counters for:
  - degenerate episode count / max streak
  - theta severity buckets
  - weak-pivot ratio buckets
  - repeated entering / leaving streaks
  - Bland pricing iterations and enter/exit episodes
  - adaptive Devex-partial iterations
  - perturb applications
  - Devex reset count / max age
  - refactors that occur while already inside a degenerate episode
- Validation:
  - `make -C ralph test-lp-telemetry-solver` passed
  - `make -C ralph build-ralph-benchmark` passed
  - `make -C ralph test-netlib-gate-small` passed
    - artifact: `/tmp/netlib-regression-gate-20260313-160703`
  - `make -C ralph test-netlib-gate` passed baseline-clean
    - `84` files
    - `22` timeouts
    - `0` status/objective/invalid mismatches
    - `0` dense fallback files
    - artifact: `/tmp/netlib-regression-gate-20260313-160716`

Focused Week 3 finding:
- the current Week 3 timeout family does **not** yet reach phase 2 on the
  current baseline
- direct focused runs:
  - `bnl1.mps`: `1083.956 ms`, `4016` iterations, `phase1.refactor_calls=647`,
    `phase2.pivot_calls=0`
  - `bnl2.mps`: `4238.978 ms`, `9831` iterations, `phase1.refactor_calls=287`,
    `phase2.pivot_calls=0`
  - `d6cube.mps`: `25534.024 ms`, `14044` iterations,
    `phase1.refactor_calls=11124`,
    `phase1.failed_stabilize_events=11004`,
    `phase1.dir_stabilize_refactor_from_force_extreme_dir=11005`,
    `phase2.pivot_calls=0`
  - `degen3.mps`: `4072.026 ms`, `19700` iterations, `phase1.refactor_calls=275`,
    `phase2.pivot_calls=0`
  - `fffff800.mps`: `3745.625 ms`, `1099` iterations, `phase1.refactor_calls=235`,
    `phase2.pivot_calls=0`
  - `greenbea.mps`: `8726.209 ms`, `27301` iterations,
    `phase1.refactor_calls=697`,
    `phase1.failed_stabilize_events=246`,
    `phase2.pivot_calls=0`
  - `maros.mps`: `1141.091 ms`, `3747` iterations,
    `phase1.refactor_calls=388`,
    `phase1.failed_stabilize_events=189`,
    `phase2.pivot_calls=0`
- focused summary TSV:
  - `/tmp/w3_phase3/summary.tsv`

Sanity check on solved phase-2-degenerate cases:
- the new phase-2 counters are live; the timeout family above is simply not in
  phase 2 yet
- `25fv47.mps`: optimal, `1587.777 ms`, `5021` phase-2 pivots,
  `degenerate_episodes=527`, `degenerate_streak_max=9`,
  `devex_reset_count=2`, `degen_refactor_calls=136`
- `scagr25.mps`: optimal, `43.423 ms`, `250` phase-2 pivots,
  `degenerate_episodes=8`, `degenerate_streak_max=8`,
  `weak_pivot_ratio_le_1e_8=3`, `degen_refactor_calls=1`

Conclusion:
- Week 3 needs to split into two tracks:
  1. current timeout family: phase-1 degenerate treadmill work first
  2. later phase-2 long-run control work on cases that actually reach phase 2
- The immediate next Week 3 slice should therefore **not** start with Devex
  weight maintenance in phase 2 for the timeout22 family.
- It should target the phase-1 degenerate treadmill on
  `d6cube` / `greenbea` / `maros`-class cases:
  - repeated `force_extreme_dir` refactors
  - repeated failed-stabilize alternates
  - no-pivot ladder churn
  - safety-refactor loops

### Week 4: Capacity / Policy Decoupling

Goal:
- finish `C2` so storage pressure does not silently rewrite effective BFCP
  policy behavior

Execution:
1. separate allocation sizing from update-policy semantics
2. provision storage to satisfy the active strict BFCP policy where feasible
3. fail explicitly when policy cannot be honored, rather than degrading
   behavior silently
4. expose capacity exhaustion and effective downgrades in telemetry

Expected payoff:
- smaller direct timeout reduction
- larger value is runtime predictability and removal of hidden policy drift

### Week 5: Numerical Certification (`xcheck`)

Goal:
- finish `D1` so hard outliers can be classified as policy failure, kernel-cost
  failure, or genuine numerical instability

Execution:
1. add final basis residual / feasibility / complementary-slackness checks
2. map doubtful end states explicitly instead of returning ambiguous failure
   buckets
3. use `xcheck` first as a strict-mode diagnostic and regression aid

Expected payoff:
- little direct timeout reduction by itself
- high diagnostic value for the last hard outliers

### Priority Order for Timeout Reduction

1. Week 1: large-basis kernel-cost parity
2. Week 2: phase-1 recompute suppression
3. Week 3: degeneracy / long-run control quality
4. Week 4: capacity / policy decoupling
5. Week 5: `xcheck`

### Promotion Rules for This Plan

1. no filename-specific tuning
2. every slice must keep `0` dense fallback regressions
3. every slice must preserve small-NETLIB green status
4. full-gate timeout reductions must persist on repeated runs before promotion

## Exit Criteria

The gap is considered closed only when all of the following are true:
- strict mode is one coherent execution lane, not a partial adapter
- LU/refactor decisions in strict mode are explainable directly from BFCP state
- remaining NETLIB outliers are dominated by kernel cost, not control drift
- strict-mode statuses/objectives are stable across repeated runs
- runtime improvements hold on the full NETLIB gate, not just a hand-picked
  subset
