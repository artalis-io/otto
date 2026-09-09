# P1-ICR: Phase-1 Iteration Control and Recompute Reduction Plan

Status: In progress  
Owner: Ralph LP  
Reference baseline: `dbeb229` (NETLIB gate timeout hygiene)

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
- [x] P1-E Pivot-failure recovery recompute hygiene.
  - Avoid redundant recompute bursts across consecutive failed recovery ladders.
  - Preserve recovery correctness and convergence safety.
- [ ] P1-F Gate and benchmark validation.
  - `make -C ralph test-lp-telemetry-solver`
  - `make -C ralph test-simplex-policy`
  - `make -C ralph test-netlib-gate-small`
  - `make -C ralph test-netlib-gate`
  - Focus telemetry reruns on `pilot`, `stair`, `degen3`.
  - Promotion rule: full gate must pass with no required-pass failures.
- [x] P1-G Markowitz retry ladder effectiveness.
  - Add explicit sparse numeric fallback reason counters in `lu_sparse.c` to separate:
    - identity-separation failure after numeric,
    - numeric backend exhaustion,
    - singular/pathological numeric failure.
  - On sparse numeric failure caused by identity-separation mismatch, force one
    full-structural sparse retry (`k=m`, no identity split) before dense fallback.
  - Make Markowitz retry-profile activation explicit and telemetry-verifiable:
    retry attempts/success/failure by profile and terminal reason.
  - Add focused unit coverage in:
    - `ralph/tests/test_lu_markowitz.c`
    - `ralph/tests/test_lp_telemetry_lu_sparse.c`
  - Gate criteria:
    - `make -C ralph test-lu-markowitz`
    - `make -C ralph test-lp-telemetry-lu-sparse`
    - `make -C ralph test-netlib-gate-small`
    - `make -C ralph test-netlib-gate`
    - required-pass `nesm.mps` must not appear in `actual.dense_fallback.txt`.

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
- [x] 2026-02-26: P1-D hardening follow-up (`fit1p` regression guard).
  - Restricted direction-skip RC-only recompute to large/high-degeneracy Phase-1 runs.
  - For small/medium cases, direction-skip now uses full recompute path (safe parity path).
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`135/135`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-202009`
    - `make -C ralph test-netlib-gate` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-202023`
- [x] 2026-02-26: P1-E implemented (pivot-failure recovery recompute hygiene).
  - Added targeted pivot-failure recovery exclusion policy:
    - after repeated pivot failures (`repeat >= 2`), temporarily exclude the unstable entering column
      during recovery-success continue paths.
    - keep recovery ladder semantics and LU-safety paths intact.
  - Added explicit telemetry:
    - `phase1_pivot_fail_recovery_exclusions`
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`138/138`)
    - `make -C ralph test-simplex-policy` PASS (`48/48`)
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-215057`
    - `make -C ralph test-netlib-gate` PASS
      - artifact: `/tmp/netlib-regression-gate-20260226-215111`
- [ ] 2026-02-27: P1-F validation run on `02f05c2` (blocked by required-pass regression).
  - Validation:
    - `make -C ralph test-lp-telemetry-solver` PASS (`156/156`)
    - `make -C ralph test-simplex-policy` PASS (`54/54`)
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260227-091415`
      - summary: 27 files, timeout files 2, status/objective/invalid mismatches 0, dense fallback files 0.
    - `make -C ralph test-netlib-gate` FAIL
      - artifact: `/tmp/netlib-regression-gate-20260227-091427`
      - summary: 84 files, timeout files 25, status/objective/invalid mismatches 0, dense fallback files 1.
      - blocker: required-pass `nesm.mps` in `actual.dense_fallback.txt` and `required.failed.txt`.
  - Focus telemetry reruns:
    - `pilot`: `/tmp/pilot_phase1_after64c154c.json`
      - timeout, 369 iterations, phase1 pivot/refactor dominates, `mkz_profile_retry_attempts=24` (all failed).
    - `stair`: `/tmp/stair_phase1_after64c154c.json`
      - timeout, 9861 iterations, ratio-recovery loop dominates (`reason_ratio_recovery=9810`).
    - `degen3`: `/tmp/degen3_phase1_after64c154c.json`
      - timeout, 9294 iterations, periodic+LU-health refactors dominate (`periodic_policy_count=37`, `periodic_lu_health_count=138`).
- [x] 2026-02-27: P1-G implemented (Markowitz retry-ladder effectiveness).
  - Added sparse numeric terminal-failure taxonomy + telemetry:
    - `identity_separation`
    - `backend_exhausted`
    - `pathological`
  - Added Markowitz retry-profile terminal-failure reason counters.
  - Added one-shot numeric full-structural sparse retry path when numeric failure
    terminal reason is identity-separation; dense fallback is now only after that
    retry also fails.
  - Added focused unit coverage:
    - `test_lu_markowitz`: numeric identity-separation full-retry regression.
    - `test_lp_telemetry_lu_sparse`: numeric terminal-reason + retry telemetry.
  - Validation:
    - `make -C ralph test-lu-markowitz` PASS (`44/44`)
    - `make -C ralph test-lp-telemetry-lu-sparse` PASS (`85/85`)
    - `make -C ralph test-netlib-gate-small` PASS
      - artifact: `/tmp/netlib-regression-gate-20260227-105533`
    - `make -C ralph test-netlib-gate` PASS
      - artifact: `/tmp/netlib-regression-gate-20260227-105548`
      - summary: 84 files, timeout files 25, dense fallback files 0, required-pass failures 0
