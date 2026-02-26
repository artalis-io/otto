# P1-ICR: Phase-1 Iteration Control and Recompute Reduction Plan

Status: In progress  
Owner: Ralph LP  
Reference baseline: `febbecd` (NETLIB gate timeout hygiene)

## Goal

Reduce Phase-1 wall time on degenerate NETLIB outliers by cutting unnecessary full-vector recomputations while preserving LU safety, status/objective correctness, and no-regression gate outcomes.

## Success Metrics

- No new status/objective/invalid mismatches on `test-netlib-gate`.
- No increase in dense fallback files.
- Reduced Phase-1 recompute pressure on hotspot cases (`pilot`, `stair`, `degen3`) measured via new telemetry counters.
- Reduced Phase-1 wall time and/or timeout count at same gate settings.

## Scope

- In scope: Phase-1 simplex control flow, recompute cadence, instrumentation, and policy gating.
- Out of scope: new numerical algorithms, exact arithmetic simplex, or major LU backend rewrites.

## Todo Plan

- [x] P1-A Telemetry foundation for recompute causes and iteration pressure.
  - Add explicit Phase-1 recompute-cause counters:
    - `ratio_breakdown_recovery`
    - `direction_skip`
    - `direction_refactor`
    - `pivot_fail_recovery`
    - `perturb_recompute`
  - Export counters via solver telemetry snapshot and benchmark JSON.
  - Add/extend unit tests in `ralph/tests/test_lp_telemetry_solver.c`.
- [x] P1-B Recompute taxonomy and guardrails.
  - Map all Phase-1 `tableau_compute_solution` and `tableau_compute_reduced_costs` call sites by cause.
  - Define safe RC-only vs full recompute invariants (per cause class).
  - Add assertions/telemetry guards to prevent stale-state drift.
- [x] P1-C Ratio-breakdown recovery tightening.
  - Reduce repeated full recompute loops when ratio breakdown repeats.
  - Keep dual-rescue and infeasibility cleanup escape hatches authoritative.
- [x] P1-D Direction-stabilize recompute decimation.
  - Keep LU-health forced refactors intact.
  - Avoid repeated full recompute on cooldown/defer loops when safe.
- [ ] P1-E Pivot-failure recovery recompute hygiene.
  - Avoid redundant recompute bursts across consecutive failed recovery ladders.
  - Preserve recovery correctness and convergence safety.
- [ ] P1-F Gate and benchmark validation.
  - `make -C ralph test-lp-telemetry-solver`
  - `make -C ralph test-simplex-policy`
  - `make -C ralph test-netlib-gate-small`
  - `make -C ralph test-netlib-gate`
  - Focus telemetry reruns on `pilot`, `stair`, `degen3`.

## Execution Log

- [x] 2026-02-26: P1-A implemented (telemetry-only, no behavior changes).
  - Added explicit Phase-1 recompute-cause counters to solver telemetry state/snapshot/public API snapshot.
  - Wired Phase-1 call-site instrumentation in `simplex.c`.
  - Exported counters in benchmark JSON.
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`117/117`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph build-ralph-benchmark` PASS
    - Single-file smoke output verified on `ralph/benchmarks/netlib/afiro.mps`.
- [x] 2026-02-26: P1-B implemented (taxonomy + guardrails).
  - Added explicit RC-only recompute telemetry:
    - `phase1_recompute_rc_only_calls`
    - `phase1_recompute_rc_guard_forced_full`
  - Added Phase-1 recompute helpers in `simplex.c` to codify invariants:
    - full recompute after basis/LU/perturbation state change
    - guarded RC-only recompute when basis/LU/primal state are unchanged
  - Added RC-only streak guard (`PHASE1_RC_ONLY_STREAK_GUARD=6`) to force full recompute and bound drift.
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`123/123`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph build-ralph-benchmark` PASS
    - Single-file smoke output verified on `ralph/benchmarks/netlib/afiro.mps`.
- [x] 2026-02-26: P1-C implemented (ratio-breakdown recovery tightening).
  - Tightened Phase-1 ratio-breakdown retries for repeated same-entering failures on large models:
    - adaptive retry limit reduction when entering repeats (`streak >= 3`).
  - Reduced soft retry cost path:
    - switched ratio-breakdown soft retry branch to guarded RC-only recompute (full recompute guard still enforced).
  - Added explicit ratio-breakdown telemetry:
    - `phase1_ratio_breakdown_retries`
    - `phase1_ratio_breakdown_escalations`
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`129/129`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph build-ralph-benchmark` PASS
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-192153`
- [x] 2026-02-26: P1-D implemented (direction-stabilize recompute decimation).
  - Converted safe direction-stabilize skip/defer paths (no LU refactor) to guarded RC-only recompute.
  - Preserved LU-health-forced refactor path and full recompute behavior for refactor-driven stabilization paths.
  - Added explicit telemetry:
    - `phase1_dir_stabilize_skip_rc_only`
    - `phase1_dir_stabilize_skip_full`
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`135/135`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph build-ralph-benchmark` PASS
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-194717`
