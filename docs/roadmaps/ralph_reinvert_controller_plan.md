# Ralph Unified Reinversion Controller: Implementation Plan

Date: 2026-03-03

## Goal

Replace scattered reinversion/refactor heuristics with one telemetry-driven controller module that is globally testable and not instance-specific.

## Guardrails (Mandatory)

- No instance-specific logic (no filename checks, no per-instance branches).
- Hard LU safety triggers remain authoritative and unchanged (`LU_FAIL_*`, singular/update safety).
- Any tuning must be based on generic runtime signals only.
- Every phase must pass regression gates before promotion.

## Baseline and Success Criteria

Baseline lock:
- Focused set: `pilot*`, `degen3`, `fit2p`.
- Full gate: `make -C ralph test-netlib-gate`.

Success criteria:
- Zero new correctness regressions.
- Zero new dense fallback regressions.
- Timeout envelope non-increasing.
- Focused set: improved or neutral `ms/iter` and `refactor.avg_ms`.

## Phase 1: Controller Scaffold (No Behavior Change)

1. Create module:
   - `ralph/include/lp_reinvert_controller.h`
   - `ralph/src/lp_reinvert_controller.c`
2. Add public internal API:
   - decision enum: `ALLOW`, `DEFER`, `FORCE`
   - stable reason codes
3. Add state struct (per phase):
   - EWMA iteration cost
   - EWMA refactor cost
   - update-age ratio
   - solve-density signal (`FTRAN/BTRAN nnz`)
   - LU-health streaks
   - cooldown window state
4. Keep pure functions only (no direct solver mutations in this phase).

## Phase 2: Shadow Wiring

1. Integrate decision calls in:
   - `ralph/src/simplex.c`
   - `ralph/src/dual_simplex.c`
2. Keep existing runtime behavior unchanged.
3. Record shadow-vs-actual decision telemetry:
   - checks
   - suggested decision
   - actual behavior
   - disagreement counters
4. Export telemetry:
   - `ralph/include/lp.h`
   - `ralph/src/lp_telemetry_solver.c`
   - `ralph/benchmarks/ralph_benchmark.c`

## Phase 3: Deterministic Unit Coverage

1. Add `ralph/tests/test_lp_reinvert_controller.c`.
2. Table-driven decision tests for:
   - warmup behavior
   - cooldown behavior
   - high refactor/iter cost ratio
   - high solve-density pressure
   - LU-health soft/hard signals
   - invalid input handling
3. Validate strict determinism for same inputs.

## Phase 4: Control Mode (Phase 1 Only)

1. Add runtime mode:
   - `off`
   - `shadow`
   - `control_phase1`
   - `control_all`
2. Enable controller-owned periodic decision for primal phase 1 only.
3. Keep hard LU safety overrides outside controller authority.
4. Run focused + full gates and compare against baseline.

## Phase 5: Control Mode Expansion (Phase 2 and Dual)

1. Extend controller-owned periodic decision to:
   - primal phase 2
   - dual periodic reinversion paths
2. Keep same hard-safety override model.
3. Validate with focused outliers then full gate.

## Phase 6: Consolidation

1. Remove duplicated periodic decision branches from:
   - `simplex.c`
   - `dual_simplex.c`
2. Reduce `lp_refactor_policy.c` to helper-only role or fold logic into controller.
3. Keep a single source of truth for non-hard reinversion decisions.

## Phase 7: Promotion

Promotion checklist:
1. `make -C ralph test-lp-reinvert-controller`
2. `make -C ralph test-simplex-policy`
3. `make -C ralph test-dual-ratio-flip`
4. Focused benchmark matrix (`pilot*`, `degen3`, `fit2p`)
5. `make -C ralph test-netlib-gate`

Promote only if:
- no new correctness regressions,
- timeout envelope is improved or neutral,
- no dense fallback regressions.

## Deliverable Strategy

Recommended commit breakdown:
1. Scaffold + shadow telemetry schema (no behavior change).
2. Unit tests for controller decisions.
3. Phase-1 control mode activation.
4. Phase-2/dual control mode activation.
5. Consolidation/removal of duplicated policy branches.

## Execution Status (2026-03-03)

- [x] Phase 1 scaffold module added (`lp_reinvert_controller`).
- [x] Deterministic unit tests added (`test_lp_reinvert_controller`).
- [x] Build and test wiring added to `ralph/Makefile`.
- [x] Phase 2 shadow wiring in simplex/dual with disagreement telemetry.
- [ ] Phase 4 control mode activation (phase 1 only).
- [ ] Phase 5 control expansion (phase 2 and dual).
