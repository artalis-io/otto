# Ralph GLPK-Strict Execution Tracker

This document is the ordered execution checklist for the three strict GLPK-parity implementation steps:

1. GLPK-style pivot quality loop
2. True working-LP `excl/shift/aorn` semantics
3. GLPK-style perturbation state machine

It is backed by a persistent state file and runner script so steps cannot be marked complete out of order.

## Tracking Files

- State file: `docs/roadmaps/ralph_glpk_strict_state.json`
- Runner: `ralph/benchmarks/glpk_strict_plan.sh`

## State Commands

From repo root:

```bash
bash ralph/benchmarks/glpk_strict_plan.sh init
bash ralph/benchmarks/glpk_strict_plan.sh status
bash ralph/benchmarks/glpk_strict_plan.sh next
```

Ordered execution:

```bash
# Phase 1
bash ralph/benchmarks/glpk_strict_plan.sh run-phase 1

# Phase 2 (only allowed after phase 1 completed)
bash ralph/benchmarks/glpk_strict_plan.sh run-phase 2

# Phase 3 (only allowed after phase 2 completed)
bash ralph/benchmarks/glpk_strict_plan.sh run-phase 3
```

Manual completion from an existing artifact:

```bash
bash ralph/benchmarks/glpk_strict_plan.sh complete <phase> --artifact /tmp/netlib-regression-gate-... --full-gate
```

## Gate Contract

Each phase completion requires:

- `make -C ralph test-simplex-policy`
- `make -C ralph test-lp-policy-glpk-compat`
- `make -C ralph test-lu-markowitz`
- `make -C ralph test-netlib-gate-small`
- `make -C ralph test-netlib-gate` (full gate; required to mark phase done)

Artifact validation requirements:

- `actual.command_failures.txt` is empty
- `unexpected.timeouts.txt` is empty
- `unexpected.status_mismatch.txt` is empty
- `unexpected.objective_mismatch.txt` is empty
- `unexpected.solution_invalid.txt` is empty
- `actual.dense_fallback.txt` is empty
- `required.failed.txt` is empty
- Full-gate completion additionally requires:
  - `missing.required.txt` is empty
  - `missing.coverage.txt` is empty

## Phase Checklist

### Phase 1: Pivot Quality Loop

- [x] Implement GLPK-like pivot quality loop module and wire in primal/dual.
- [x] Add orthogonal unit tests for loop behavior.
- [x] Run phase gate and mark phase complete in tracker.

### Phase 2: Working-LP Semantics

- [x] Implement true working-LP transform semantics for `excl`, `shift`, and `aorn`.
- [x] Add mapping/round-trip/unit parity tests.
- [x] Run phase gate and mark phase complete in tracker.

### Phase 3: Perturbation State Machine

- [ ] Implement perturbation state machine with explicit transitions.
- [ ] Add state-transition unit tests and degeneracy regression checks.
- [ ] Run phase gate and mark phase complete in tracker.

## Notes

- This tracker is intentionally strict: phase N cannot start until phase N-1 is `completed` in state.
- Use `--small-only` only for local iteration; it does not mark a phase completed.
