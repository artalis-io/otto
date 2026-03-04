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
- `glpk_smcp_method`: `auto` (0), `primal` (1), `dual` (2)
- `glpk_smcp_pricing`: `standard` (0), `steep` (1)
- `glpk_smcp_ratio`: `standard` (0), `harris` (1)
- `glpk_smcp_flip`: `off` (0), `on` (1)
- `glpk_smcp_basis`: `adv` (0), `std` (1)
- `glpk_smcp_presolve`: `auto` (0), `off` (1), `on` (2)

3. BFCP-like controls
- `glpk_bfcp_backend`: `luf_ft` (0), `cbg` (1), `cgr` (2)
- `glpk_bfcp_update_limit`: integer (`-1` = auto/default)
- `glpk_bfcp_pivot_tol`: double (`<=0` = auto/default)
- `glpk_bfcp_growth_guard`: double (`<=0` = auto/default)

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
   - backend support/clamp (`luf_ft` currently supported)
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
