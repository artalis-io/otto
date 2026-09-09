# Ralph vs GLPK: Basis Management and Performance Plan

## Purpose

This document captures:
- The current Ralph LP performance gap vs GLPK.
- The architectural reason for the gap.
- A concrete implementation path to close it without breaking correctness gates.

## Reference Baseline (2026-02-25)

- Branch: `feature/724da701-ralph-mip-infrastructure`
- Baseline commit: `f5c3020`
- NETLIB gate artifact: `/tmp/netlib-regression-gate-20260225-120035`
- Gate status:
  - `84/84` processed
  - `timeouts=26`
  - `status/objective/invalid mismatches=0`
  - `dense fallback files=0`

Focused telemetry snapshot at this baseline:
- `pilot.mps`: low iteration count but high refactor/event cost.
- `stair.mps`: iteration explosion dominates runtime.

## GLPK Reference Model

GLPK separates control into two planes:

1. Simplex control (`glp_smcp`)
- method (`primal`, `dual`)
- pricing (`standard`, `projected steepest-edge`)
- ratio test (`standard`, `Harris`, `long-step/flip-flop`)
- tolerances and limits
- presolve toggle

2. Basis factorization control (`glp_bfcp`)
- factorization/update backend selection (`LUF/FT`, `BG`, `GR`)
- pivot tolerances and update controls

CLI signals the same design:
- `--dual`
- `--steep` / `--nosteep`
- `--relax` / `--norelax`
- `--flip`
- `--luf --ft`, `--cbg`, `--cgr`
- `--adv` / `--std`
- `--presol` / `--nopresol`
- `--exact`

## Current Ralph Architecture Snapshot

Ralph has strong components, but the control policy is fragmented:

- Refactor policy logic exists and is rich (`ralph/src/lp_refactor_policy.c`).
- Primal simplex consumes many policy signals (`ralph/src/simplex.c`).
- Dual simplex still has hard-coded cadence paths (for example `% 50` periodic behavior in `ralph/src/dual_simplex.c`).
- Sparse LU backend routing is mostly local/reactive (`ralph/src/lu_sparse.c`), even with new telemetry.

Result: local heuristics can improve one family (`stair`) while regressing another (`pilot`).

## Core Gap vs GLPK

The main missing piece is a single global basis governor that jointly optimizes:

- iteration progress quality (anti-degeneracy and pivot progression),
- reinversion/refactor frequency and timing,
- LU backend selection on structure-local histories,
- safety triggers (numerical health) with hard priority.

Today, these decisions are split across loops and modules with partial context.

## Target Architecture

Add a dedicated module:
- `ralph/include/lp_basis_governor.h`
- `ralph/src/lp_basis_governor.c`

With an embedded state:
- `LPBasisGovernorState` inside `LPSolverPolicyState`.

Governor produces unified decisions:
- `refactor_now` and reason,
- pricing/ratio mode hint,
- perturbation action,
- full-retry LU backend ordering (`Markowitz`, `supernode`, `dense`),
- confidence/safety override tags.

Safety contract:
- hard numerical triggers always override optimization logic.

## First Implementation Step: G0 (Shadow Governor Baseline)

Goal: introduce the governor without behavior change.

### Scope
- Add governor state and API.
- Feed it the same metrics currently used by simplex/dual/LU policy paths.
- Run in shadow mode only: decisions are logged to telemetry but not executed.

### Required Interfaces
- `lp_basis_governor_begin_solve(...)`
- `lp_basis_governor_observe_iter(...)`
- `lp_basis_governor_observe_refactor(...)`
- `lp_basis_governor_observe_lu_backend(...)`
- `lp_basis_governor_shadow_decide(...)`

### Telemetry Additions
- shadow decision counts by phase and reason:
  - `shadow_refactor_yes/no`
  - `shadow_reason_*`
- shadow backend ordering picks:
  - `shadow_backend_pick_markowitz`
  - `shadow_backend_pick_supernode`
  - `shadow_backend_pick_dense`
- disagreement counters:
  - `shadow_disagree_primal_refactor`
  - `shadow_disagree_dual_refactor`
  - `shadow_disagree_lu_backend`

### Tests
- Unit tests (new):
  - `ralph/tests/test_lp_basis_governor.c`
  - deterministic state transition and shadow decision assertions.
- No-regression gates (must stay green):
  - `make -C ralph test-lu-markowitz`
  - `make -C ralph test-simplex-policy`
  - `make -C ralph test-netlib-gate-small`
  - `make -C ralph test-netlib-gate`

### Acceptance Criteria
- Zero solver behavior change relative to baseline.
- All existing gates unchanged.
- Shadow telemetry present in benchmark JSON for offline comparison.

## G1 Entry Criteria (After G0)

Only after G0 telemetry is stable:
- Enable governor-controlled refactor decision in primal phase 2 only.
- Keep dual and LU backend ordering in shadow mode.
- Abort/rollback switch available via runtime param if mismatch risk appears.
